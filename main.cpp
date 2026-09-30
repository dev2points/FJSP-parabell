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
#include <cerrno>
#include <csignal>
#include <cstring>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
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
    long long preferred_bound = -1;
    pid_t pid = -1;
    int fd = -1;
    std::vector<char> buffer;
};

static bool write_all(int fd, const void *data, size_t size)
{
    const char *bytes = static_cast<const char *>(data);
    while (size > 0)
    {
        ssize_t written = write(fd, bytes, size);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return false;
        bytes += written;
        size -= static_cast<size_t>(written);
    }
    return true;
}

static BoundResult read_result(const std::vector<char> &buffer)
{
    BoundResult result;
    int status = 2;
    int model_size = 0;
    std::memcpy(&status, buffer.data(), sizeof(status));
    std::memcpy(&model_size, buffer.data() + sizeof(status), sizeof(model_size));
    result.status = status == 0 ? "SAT" : status == 1 ? "UNSAT"
                                                      : "TIMEOUT";
    if (status == 0 && model_size > 0)
    {
        result.model.resize(static_cast<size_t>(model_size));
        std::memcpy(result.model.data(), buffer.data() + sizeof(status) + sizeof(model_size),
                    result.model.size() * sizeof(int));
    }
    return result;
}

static void stop_worker(WorkerSlot &slot)
{
    if (slot.busy && slot.pid > 0)
        kill(slot.pid, SIGTERM);
}

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
            long long cand = -1;
            if (slot.preferred_bound >= lo && slot.preferred_bound <= upper &&
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
                cand = pick_candidate(intervals, LLONG_MAX);
            if (cand < 0)
                break;
            slot.bound = cand;
            slot.busy = true;
            slot.buffer.clear();

            int pipe_fds[2];
            if (pipe(pipe_fds) != 0)
                throw std::runtime_error("Khong tao duoc pipe cho worker process");
            slot.pid = fork();
            if (slot.pid < 0)
            {
                close(pipe_fds[0]);
                close(pipe_fds[1]);
                throw std::runtime_error("Khong tao duoc worker process");
            }
            if (slot.pid == 0)
            {
                close(pipe_fds[0]);
                auto cancel = std::make_shared<std::atomic<bool>>(false);
                IncrementalBoundSolver session(inst, m);
                BoundResult result = session.solve(cand, deadline, cancel);
                int status = result.status == "SAT" ? 0 : result.status == "UNSAT" ? 1
                                                                                   : 2;
                int model_size = static_cast<int>(result.model.size());
                write_all(pipe_fds[1], &status, sizeof(status));
                write_all(pipe_fds[1], &model_size, sizeof(model_size));
                if (model_size > 0)
                    write_all(pipe_fds[1], result.model.data(), result.model.size() * sizeof(int));
                close(pipe_fds[1]);
                _exit(0);
            }
            close(pipe_fds[1]);
            slot.fd = pipe_fds[0];
            log("[START] UB=" + std::to_string(cand));
        }
    };

    start_available();
    while (true)
    {
        bool any_busy = false;
        std::vector<struct pollfd> poll_fds;
        for (auto &s : slots)
            if (s.busy)
            {
                any_busy = true;
                poll_fds.push_back({s.fd, POLLIN | POLLHUP, 0});
            }
        if (!any_busy)
            break;
        if (proven || failed)
            break;
        if (Clock::now() >= deadline)
            break;

        if (!poll_fds.empty())
            poll(poll_fds.data(), poll_fds.size(), 5);
        bool progressed = false;
        size_t poll_index = 0;
        for (auto &slot : slots)
        {
            if (!slot.busy)
                continue;
            struct pollfd &event = poll_fds[poll_index++];
            if (!(event.revents & (POLLIN | POLLHUP)))
                continue;
            char chunk[8192];
            bool eof = false;
            while (true)
            {
                ssize_t count = read(slot.fd, chunk, sizeof(chunk));
                if (count > 0)
                    slot.buffer.insert(slot.buffer.end(), chunk, chunk + count);
                else if (count < 0 && errno == EINTR)
                    continue;
                else
                {
                    eof = count == 0;
                    break;
                }
            }
            if (slot.buffer.size() < sizeof(int) * 2)
            {
                if (!eof)
                    continue;
                slot.buffer.assign(sizeof(int) * 2, 0);
                int timeout_status = 2;
                std::memcpy(slot.buffer.data(), &timeout_status, sizeof(timeout_status));
            }
            int model_size = 0;
            std::memcpy(&model_size, slot.buffer.data() + sizeof(int), sizeof(model_size));
            size_t expected_size = sizeof(int) * 2 + static_cast<size_t>(std::max(0, model_size)) * sizeof(int);
            if (slot.buffer.size() < expected_size)
            {
                if (!eof)
                    continue;
                std::fill(slot.buffer.begin(), slot.buffer.end(), 0);
                int timeout_status = 2;
                std::memcpy(slot.buffer.data(), &timeout_status, sizeof(timeout_status));
            }
            BoundResult res = read_result(slot.buffer);
            long long bound = slot.bound;
            slot.busy = false;
            close(slot.fd);
            waitpid(slot.pid, nullptr, 0);
            slot.fd = -1;
            slot.pid = -1;
            progressed = true;
            tested.insert(bound);

            if (bound < lo || bound > upper)
            {
                if (res.status != "SAT")
                {
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
                        stop_worker(s2);
            }
            else if (res.status == "UNSAT")
            {
                slot.preferred_bound = -1;
                lo = std::max(lo, bound + 1);
                log("[UNSAT] UB=" + std::to_string(bound) +
                    "  | LB=" + std::to_string(lo) + " UB=" + std::to_string(out.has_best ? out.cmax : upper + 1));
                for (auto &s2 : slots)
                    if (s2.busy && s2.bound <= bound)
                        stop_worker(s2);
            }
            else
            {
                slot.preferred_bound = -1;
                log("[TIMEOUT] UB=" + std::to_string(bound));
            }

            if (lo > upper)
            {
                proven = out.has_best;
                break;
            }
        }

        (void)progressed;
        if (lo <= upper && !proven && Clock::now() < deadline)
            start_available();
    }

    // Dung va thu hoi cac process con dang chay.
    for (auto &s : slots)
    {
        if (s.busy)
        {
            stop_worker(s);
            close(s.fd);
            waitpid(s.pid, nullptr, 0);
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
    int workers = 4;
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
