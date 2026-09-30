// main.cpp
// Giai bai toan Flexible Job Shop Scheduling voi Setup Time, Transport Time
// va Assembly/Component Operations bang SAT (Order Encoding), dung CaDiCaL.
//
// Cach dung:
//   ./fjsp_sat <instance.json> [--time-limit 600] [--output out.json]
//              [--ub N] [--seed 0] [--workers 4]
//
// Chien luoc: tim upper bound bang heuristic tham lam ngau nhien, sau do tim
// kiem nhi phan tren Cmax; moi bound duoc giai boi mot the hien CaDiCaL::Solver
// doc lap, cac bound duoc thu song song (toi da --workers cai cung luc) va
// huy hop tac (Terminator) ngay khi bi mot ket qua khac lam cho vo nghia.
#include "instance.hpp"
#include "cnf_encoder.hpp"
#include "cadical_worker.hpp"

#include <iostream>
#include <fstream>
#include <sstream>
#include <future>
#include <thread>
#include <unordered_set>
#include <algorithm>
#include <iomanip>
#include <cstdlib>

using Clock = std::chrono::steady_clock;

// ----------------------------------------------------------------------------
// Tim kiem nhi phan song song
// ----------------------------------------------------------------------------

struct SearchOutcome
{
    bool has_best = false;
    long long cmax = 0;
    std::vector<long long> S;
    std::vector<std::string> M;
    bool proven = false;
    long long lo = 0;
};

struct WorkerSlot
{
    bool busy = false;
    long long bound = -1;
    std::shared_future<BoundResult> fut;
    std::shared_ptr<std::atomic<bool>> cancel;
    std::shared_ptr<IncrementalBoundSolver> session;
    long long session_bound = LLONG_MAX;
    long long preferred_bound = -1;
};

static SearchOutcome parallel_binary_search(
    const Instance &inst, const CnfModel &m,
    long long lo, long long upper, int workers,
    Clock::time_point deadline,
    bool have_best, long long best_cmax,
    std::vector<long long> best_S, std::vector<std::string> best_M,
    bool verbose)
{
    auto log = [&](const std::string &s)
    { if (verbose) std::cout << s << std::endl; };

    SearchOutcome out;
    out.has_best = have_best;
    out.cmax = best_cmax;
    out.S = best_S;
    out.M = best_M;
    out.lo = lo;

    if (upper < lo)
    {
        out.proven = have_best;
        return out;
    }

    std::vector<WorkerSlot> slots(std::max(1, workers));
    std::unordered_set<long long> tested;
    bool proven = false, failed = false;

    // Tim mot bound chua thu / chua chay, chia khoang theo kieu 3/4 (giong Python).
    auto pick_candidate = [&](std::vector<std::pair<long long, long long>> &intervals,
                              long long session_bound) -> long long
    {
        while (!intervals.empty())
        {
            size_t best_idx = 0;
            long long best_w = -1;
            for (size_t idx = 0; idx < intervals.size(); idx++)
            {
                long long w = intervals[idx].second - intervals[idx].first;
                if (w > best_w)
                {
                    best_w = w;
                    best_idx = idx;
                }
            }
            auto seg = intervals[best_idx];
            intervals.erase(intervals.begin() + best_idx);
            long long left = seg.first, right = seg.second;
            if (left > right)
                continue;
            long long mid = left + (3 * (right - left)) / 4;
            intervals.push_back({left, mid - 1});
            intervals.push_back({mid + 1, right});
            bool is_active = false;
            for (auto &s : slots)
                if (s.busy && s.bound == mid)
                {
                    is_active = true;
                    break;
                }
            if (!tested.count(mid) && !is_active && mid < session_bound)
                return mid;
        }
        return -1;
    };

    auto start_available = [&]()
    {
        if (upper < lo)
            return;
        std::vector<std::pair<long long, long long>> intervals = {{lo, upper}};
        for (auto &slot : slots)
        {
            if (slot.busy)
                continue;
            if (slot.session && slot.session_bound <= lo)
            {
                slot.session.reset();
                slot.session_bound = LLONG_MAX;
            }
            long long cand = -1;
            if (slot.preferred_bound >= lo && slot.preferred_bound <= upper &&
                slot.preferred_bound < slot.session_bound &&
                !tested.count(slot.preferred_bound))
            {
                bool is_active = false;
                for (auto &other : slots)
                    if (other.busy && other.bound == slot.preferred_bound)
                    {
                        is_active = true;
                        break;
                    }
                if (!is_active)
                    cand = slot.preferred_bound;
            }
            slot.preferred_bound = -1;
            if (cand < 0)
                cand = pick_candidate(intervals, slot.session_bound);
            if (cand < 0)
                break;
            if (!slot.session)
                slot.session = std::make_shared<IncrementalBoundSolver>(inst, m);
            slot.bound = cand;
            slot.session_bound = cand;
            slot.busy = true;
            slot.cancel = std::make_shared<std::atomic<bool>>(false);
            auto cancel_ptr = slot.cancel;
            auto session = slot.session;
            long long b = cand;
            slot.fut = std::async(std::launch::async, [session, b, deadline, cancel_ptr]()
                                  { return session->solve(b, deadline, cancel_ptr); })
                           .share();
            log("[START] UB=" + std::to_string(cand));
        }
    };

    start_available();
    while (true)
    {
        bool any_busy = false;
        for (auto &s : slots)
            if (s.busy)
                any_busy = true;
        if (!any_busy)
            break;
        if (proven || failed)
            break;
        if (Clock::now() >= deadline)
            break;

        bool progressed = false;
        for (auto &slot : slots)
        {
            if (!slot.busy)
                continue;
            if (slot.fut.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
                continue;
            BoundResult res = slot.fut.get();
            long long bound = slot.bound;
            slot.busy = false;
            slot.cancel.reset();
            progressed = true;
            tested.insert(bound);

            if (bound < lo || bound > upper)
            {
                if (res.status != "SAT")
                {
                    slot.session.reset();
                    slot.session_bound = LLONG_MAX;
                    slot.preferred_bound = -1;
                }
                continue; // ket qua da loi thoi (stale)
            }

            if (res.status == "SAT")
            {
                Schedule sch = decode(inst, res.model, m);
                VerifyResult vr = verify(inst, sch.S, sch.M);
                if (!vr.ok)
                {
                    log("[CANH BAO] Loi giai SAT cho UB=" + std::to_string(bound) + " khong qua kiem tra.");
                    continue;
                }
                if (!out.has_best || vr.cmax < out.cmax)
                {
                    out.has_best = true;
                    out.cmax = vr.cmax;
                    out.S = sch.S;
                    out.M = sch.M;
                }
                upper = std::min(upper, vr.cmax - 1);
                slot.preferred_bound = upper;
                log("[SAT]   UB=" + std::to_string(bound) + " -> Cmax=" + std::to_string(vr.cmax) +
                    "  | LB=" + std::to_string(lo) + " UB=" + std::to_string(out.cmax));
                for (auto &s2 : slots)
                    if (s2.busy && s2.bound >= vr.cmax)
                        s2.cancel->store(true);
            }
            else if (res.status == "UNSAT")
            {
                slot.session.reset();
                slot.session_bound = LLONG_MAX;
                slot.preferred_bound = -1;
                lo = std::max(lo, bound + 1);
                log("[UNSAT] UB=" + std::to_string(bound) +
                    "  | LB=" + std::to_string(lo) + " UB=" + std::to_string(out.has_best ? out.cmax : upper + 1));
                for (auto &s2 : slots)
                    if (s2.busy && s2.bound <= bound)
                        s2.cancel->store(true);
                for (auto &s2 : slots)
                {
                    if (!s2.busy && s2.session && s2.session_bound <= bound)
                    {
                        s2.session.reset();
                        s2.session_bound = LLONG_MAX;
                    }
                }
            }
            else
            {
                slot.session.reset();
                slot.session_bound = LLONG_MAX;
                slot.preferred_bound = -1;
                log("[TIMEOUT] UB=" + std::to_string(bound));
            }

            if (lo > upper)
            {
                proven = out.has_best;
                break;
            }
        }

        if (!progressed)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (lo <= upper && !proven && Clock::now() < deadline)
            start_available();
    }

    // Cho cac slot con dang chay ket thuc (huy hop tac truoc, khong the kill cung), roi bo qua ket qua.
    for (auto &s : slots)
    {
        if (s.busy)
        {
            if (s.cancel)
                s.cancel->store(true);
            s.fut.wait();
        }
    }

    out.proven = proven || (lo > upper && out.has_best);
    out.lo = lo;
    return out;
}

// ----------------------------------------------------------------------------
// Xuat ket qua
// ----------------------------------------------------------------------------

static std::string json_escape(const std::string &s)
{
    std::string out;
    for (char c : s)
    {
        if (c == '"' || c == '\\')
            out += '\\';
        out += c;
    }
    return out;
}

static void report(const Instance &inst, bool has_best, long long cmax,
                   const std::vector<long long> &S, const std::vector<std::string> &M,
                   bool proven, long long lb, double elapsed, const std::string &out_path)
{
    if (!has_best)
    {
        std::cout << "[Loi] Khong tim duoc lich kha thi.\n";
        return;
    }
    std::cout << "\n"
              << std::string(72, '=') << "\n";
    std::cout << "Makespan = " << cmax << "   ("
              << (proven ? "TOI UU (da chung minh)" : "kha thi, chua chung minh toi uu")
              << ")   |   thoi gian = " << std::fixed << std::setprecision(2) << elapsed << "s\n";
    std::cout << std::string(72, '=') << "\n";

    std::vector<int> order(inst.n);
    for (int i = 0; i < inst.n; i++)
        order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](int a, int b)
                     { return S[a] < S[b]; });

    std::cout << std::left
              << std::setw(22) << "Operation" << std::setw(8) << "Job"
              << std::setw(18) << "Type" << std::setw(12) << "May"
              << std::right << std::setw(7) << "Start" << std::setw(7) << "End" << "\n";
    for (int i : order)
    {
        std::cout << std::left
                  << std::setw(22) << inst.op_names[i] << std::setw(8) << inst.job[i]
                  << std::setw(18) << inst.type[i] << std::setw(12) << M[i]
                  << std::right << std::setw(7) << S[i]
                  << std::setw(7) << (S[i] + inst.p[i].at(M[i])) << "\n";
    }

    VerifyResult vr = verify(inst, S, M);
    std::cout << "\nKiem tra doc lap: " << (vr.ok ? "HOP LE" : "LOI") << "\n";
    if (!vr.ok)
        for (auto &e : vr.errors)
            std::cout << "  - " << e << "\n";

    if (!out_path.empty())
    {
        std::ofstream f(out_path);
        f << "{\n";
        f << "  \"makespan\": " << cmax << ",\n";
        f << "  \"proven_optimal\": " << (proven ? "true" : "false") << ",\n";
        f << "  \"lower_bound\": " << lb << ",\n";
        f << "  \"runtime_seconds\": " << std::fixed << std::setprecision(3) << elapsed << ",\n";
        f << "  \"schedule\": [\n";
        for (size_t idx = 0; idx < order.size(); idx++)
        {
            int i = order[idx];
            f << "    {\"operation\": \"" << json_escape(inst.op_names[i]) << "\", "
              << "\"job\": \"" << json_escape(inst.job[i]) << "\", "
              << "\"type\": \"" << json_escape(inst.type[i]) << "\", "
              << "\"resource\": \"" << json_escape(M[i]) << "\", "
              << "\"start\": " << S[i] << ", "
              << "\"end\": " << (S[i] + inst.p[i].at(M[i])) << "}"
              << (idx + 1 < order.size() ? "," : "") << "\n";
        }
        f << "  ]\n}\n";
        std::cout << "Da ghi ket qua vao " << out_path << "\n";
    }
}

// ----------------------------------------------------------------------------
// main
// ----------------------------------------------------------------------------

int main(int argc, char **argv)
{
    std::string instance_path;
    double time_limit = 600.0;
    std::string output_path;
    bool have_ub_hint = false;
    long long ub_hint = -1;
    unsigned seed = 0;
    int workers = (int)std::thread::hardware_concurrency();
    if (workers <= 0)
        workers = 4;
    workers = std::min(workers, 4);

    std::vector<std::string> args(argv + 1, argv + argc);
    for (size_t i = 0; i < args.size(); i++)
    {
        const std::string &a = args[i];
        if (a == "--time-limit" && i + 1 < args.size())
            time_limit = std::stod(args[++i]);
        else if ((a == "--output" || a == "-o") && i + 1 < args.size())
            output_path = args[++i];
        else if (a == "--ub" && i + 1 < args.size())
        {
            ub_hint = std::stoll(args[++i]);
            have_ub_hint = true;
        }
        else if (a == "--seed" && i + 1 < args.size())
            seed = (unsigned)std::stoul(args[++i]);
        else if (a == "--workers" && i + 1 < args.size())
            workers = std::stoi(args[++i]);
        else if (!a.empty() && a[0] != '-')
            instance_path = a;
        else
        {
            std::cerr << "Tham so khong hop le: " << a << "\n";
            return 1;
        }
    }
    if (instance_path.empty())
    {
        std::cerr << "Cach dung: fjsp_sat <instance.json> [--time-limit S] [--output F] "
                     "[--ub N] [--seed N] [--workers N]\n";
        return 1;
    }

    Instance inst = load_instance(instance_path);
    std::unordered_set<std::string> jobs(inst.job.begin(), inst.job.end());
    std::cout << "Instance: " << jobs.size() << " job, " << inst.n << " operation, "
              << inst.all_machines.size() << " may\n";

    auto t_start = Clock::now();
    auto deadline = t_start + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(time_limit));

    long long lb = lower_bound(inst);
    double heur_budget = std::min(3.0, time_limit * 0.1);
    GreedyBest heur = find_upper_bound(inst, seed, heur_budget);

    bool have_best = heur.ok;
    long long best_cmax = 0;
    std::vector<long long> best_S;
    std::vector<std::string> best_M;
    if (heur.ok)
    {
        best_cmax = heur.cmax;
        best_S = heur.S;
        best_M = heur.M;
        std::cout << "[Heuristic] Upper bound ban dau = " << best_cmax
                  << "   |   Lower bound = " << lb << "\n";
    }
    if (!have_best && !have_ub_hint)
    {
        std::cerr << "[Loi] Khong tim duoc lich kha thi ban dau (kiem tra du lieu).\n";
        return 1;
    }

    long long initial_upper = have_best ? best_cmax : ub_hint;
    if (have_ub_hint && (!have_best || ub_hint < initial_upper))
    {
        initial_upper = ub_hint;
        if (have_best && best_cmax > initial_upper)
            have_best = false;
    }

    long long lo = lb;
    workers = std::max(1, workers);
    std::cout << "[Info] Su dung " << workers << " bo giai SAT (bound) song song.\n";

    auto built = build_cnf(inst, initial_upper);
    double elapsed;
    if (!built.has_value())
    {
        elapsed = std::chrono::duration<double>(Clock::now() - t_start).count();
        report(inst, have_best, best_cmax, best_S, best_M, false, lb, elapsed, output_path);
        return 0;
    }
    CnfModel &m = *built;
    long long search_upper = have_best ? initial_upper - 1 : initial_upper;

    SearchOutcome res = parallel_binary_search(inst, m, lo, search_upper, workers, deadline,
                                               have_best, best_cmax, best_S, best_M, true);

    elapsed = std::chrono::duration<double>(Clock::now() - t_start).count();
    report(inst, res.has_best, res.cmax, res.S, res.M, res.proven, lb, elapsed, output_path);
    return 0;
}
