// instance.hpp
// Doc instance FJSP+ tu JSON, cac tien ich thoi gian (setup/transport/lag),
// kiem tra loi giai doc lap (verify), va heuristic tham lam de tim upper bound.
// Tuong ung truc tiep voi class Instance / verify() / greedy_schedule() /
// find_upper_bound() / lower_bound() trong bisec.py.
#pragma once
#include "json_min.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <random>
#include <chrono>
#include <climits>
#include <iostream>

struct Instance {
    std::vector<std::string> op_names;
    int n = 0;
    std::unordered_map<std::string, int> idx;

    std::vector<std::string> job;   // job cua tung operation
    std::vector<std::string> type;  // operation_type cua tung operation
    std::vector<char> asm_op;       // 1: assembly, 0: component (mac dinh 1)

    std::vector<std::unordered_map<std::string, long long>> p; // p[i][machine] = thoi gian xu ly
    std::vector<std::vector<std::string>> machines;             // may tuong thich, da sap xep

    std::vector<std::string> all_machines;
    std::unordered_map<std::string, long long> release; // release[machine]

    // setup[type_i][type_j][machine] = setup time (giu nguyen dang nested-map nhu JSON)
    std::unordered_map<std::string,
        std::unordered_map<std::string, std::unordered_map<std::string, long long>>> setup_raw;
    // transport[k1][k2] = transport time
    std::unordered_map<std::string, std::unordered_map<std::string, long long>> transport_raw;

    std::vector<std::vector<int>> succ, pred; // do thi uu tien (DAG), theo chi so operation
    std::vector<int> topo;
    std::vector<std::vector<bool>> desc; // bao dong bac cau: desc[i][j] = true neu j la hau due cua i

    // ---- tien ich thoi gian ----
    long long st(int i, int j, const std::string& k) const {
        if (type[i] == type[j]) return 0;
        auto it1 = setup_raw.find(type[i]);
        if (it1 == setup_raw.end()) return 0;
        auto it2 = it1->second.find(type[j]);
        if (it2 == it1->second.end()) return 0;
        auto it3 = it2->second.find(k);
        if (it3 == it2->second.end()) return 0;
        return it3->second;
    }
    long long tt(const std::string& k1, const std::string& k2) const {
        if (k1 == k2) return 0;
        auto it1 = transport_raw.find(k1);
        if (it1 == transport_raw.end()) return 0;
        auto it2 = it1->second.find(k2);
        if (it2 == it1->second.end()) return 0;
        return it2->second;
    }
    // Khoang cach toi thieu S_j - S_i khi i chay truoc j (i tren k1, j tren k2).
    long long lag(int i, int j, const std::string& k1, const std::string& k2) const {
        long long proc = p[i].at(k1);
        return proc + (k1 == k2 ? st(i, j, k1) : tt(k1, k2));
    }
    // Cap (i,j) khong co quan he DAG co can rang buoc disjunctive khi gan (k1,k2)?
    bool need_pair(int i, int j, const std::string& k1, const std::string& k2) const {
        if (k1 == k2) return true; // khong chong lap + setup tren cung may
        return job[i] == job[j] && asm_op[i] && asm_op[j]; // van chuyen lien may
    }
    bool related(int i, int j) const { return desc[i][j] || desc[j][i]; }

    long long min_edge_lag(int i, int j) const {
        long long best = LLONG_MAX;
        for (auto& k1 : machines[i])
            for (auto& k2 : machines[j])
                best = std::min(best, lag(i, j, k1, k2));
        return best;
    }
};

inline Instance load_instance(const std::string& path) {
    using namespace json_min;
    Json d = parse_file(path);
    Instance inst;

    for (auto& o : d.at("operations").arr) inst.op_names.push_back(o.as_string());
    inst.n = (int) inst.op_names.size();
    for (int i = 0; i < inst.n; i++) inst.idx[inst.op_names[i]] = i;

    inst.job.resize(inst.n);
    inst.type.resize(inst.n);
    inst.asm_op.assign(inst.n, 1);

    const Json& oj = d.at("operations_jobs");
    const Json& ot = d.at("operations_operation_types");
    for (int i = 0; i < inst.n; i++) {
        inst.job[i] = oj.at(inst.op_names[i]).as_string();
        inst.type[i] = ot.at(inst.op_names[i]).as_string();
    }
    if (const Json* oc = d.find("operation_classes")) {
        for (int i = 0; i < inst.n; i++) {
            const Json* v = oc->find(inst.op_names[i]);
            if (v && v->is_number()) inst.asm_op[i] = v->as_bool() ? 1 : 0;
        }
    }

    inst.p.resize(inst.n);
    inst.machines.resize(inst.n);
    const Json& pts = d.at("processing_times");
    for (int i = 0; i < inst.n; i++) {
        const Json& row = pts.at(inst.op_names[i]);
        if (!row.is_object() || row.obj.empty()) {
            std::cerr << "[Loi] Operation " << inst.op_names[i] << " khong co may tuong thich.\n";
            std::exit(1);
        }
        for (auto& kv : row.obj) {
            inst.p[i][kv.first] = kv.second.as_ll();
            inst.machines[i].push_back(kv.first);
        }
        std::sort(inst.machines[i].begin(), inst.machines[i].end());
    }

    for (auto& m : d.at("resources").arr) inst.all_machines.push_back(m.as_string());
    if (const Json* bt = d.find("blocked_times")) {
        for (auto& k : inst.all_machines) {
            const Json* v = bt->find(k);
            inst.release[k] = v ? v->as_ll() : 0;
        }
    } else {
        for (auto& k : inst.all_machines) inst.release[k] = 0;
    }

    if (const Json* su = d.find("setup_times")) {
        for (auto& [ti, row1] : su->obj) {
            for (auto& [tj, row2] : row1.obj) {
                for (auto& [k, val] : row2.obj) {
                    if (val.is_number()) inst.setup_raw[ti][tj][k] = val.as_ll();
                }
            }
        }
    }
    if (const Json* tr = d.find("transport_times")) {
        for (auto& [k1, row] : tr->obj) {
            for (auto& [k2, val] : row.obj) {
                if (val.is_number()) inst.transport_raw[k1][k2] = val.as_ll();
            }
        }
    }

    inst.succ.assign(inst.n, {});
    inst.pred.assign(inst.n, {});
    if (const Json* pc = d.find("precedence_constraints")) {
        for (auto& [jobname, mat] : pc->obj) {
            (void) jobname;
            for (auto& [o, row] : mat.obj) {
                if (!inst.idx.count(o)) continue;
                int a = inst.idx[o];
                for (auto& [u, v] : row.obj) {
                    if (!v.is_number() || v.as_ll() != 1) continue;
                    if (u == o) continue;
                    if (!inst.idx.count(u)) continue;
                    int b = inst.idx[u];
                    bool exists = false;
                    for (int x : inst.succ[a]) if (x == b) { exists = true; break; }
                    if (!exists) { inst.succ[a].push_back(b); inst.pred[b].push_back(a); }
                }
            }
        }
    }

    // Sap xep topo + bao dong bac cau
    std::vector<int> indeg(inst.n);
    for (int i = 0; i < inst.n; i++) indeg[i] = (int) inst.pred[i].size();
    std::vector<int> order;
    for (int i = 0; i < inst.n; i++) if (indeg[i] == 0) order.push_back(i);
    for (size_t h = 0; h < order.size(); h++) {
        int u = order[h];
        for (int v : inst.succ[u]) if (--indeg[v] == 0) order.push_back(v);
    }
    if ((int) order.size() != inst.n) {
        std::cerr << "[Loi] Do thi uu tien co chu trinh (khong phai DAG).\n";
        std::exit(1);
    }
    inst.topo = order;
    inst.desc.assign(inst.n, std::vector<bool>(inst.n, false));
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        int u = *it;
        for (int v : inst.succ[u]) {
            inst.desc[u][v] = true;
            for (int w = 0; w < inst.n; w++) if (inst.desc[v][w]) inst.desc[u][w] = true;
        }
    }
    return inst;
}

// ----------------------------------------------------------------------------
// Kiem tra loi giai doc lap
// ----------------------------------------------------------------------------
struct VerifyResult {
    bool ok = false;
    long long cmax = 0;
    std::vector<std::string> errors;
};

inline VerifyResult verify(const Instance& inst, const std::vector<long long>& S,
                            const std::vector<std::string>& M) {
    VerifyResult r;
    for (int i = 0; i < inst.n; i++) {
        if (!inst.p[i].count(M[i]))
            r.errors.push_back(inst.op_names[i] + ": may " + M[i] + " khong tuong thich");
        else if (S[i] < inst.release.at(M[i]))
            r.errors.push_back(inst.op_names[i] + ": bat dau truoc release cua " + M[i]);
    }
    for (int i = 0; i < inst.n; i++) {
        for (int j : inst.succ[i]) {
            if (S[j] < S[i] + inst.lag(i, j, M[i], M[j]))
                r.errors.push_back("Precedence " + inst.op_names[i] + " -> " + inst.op_names[j] + " bi vi pham");
        }
    }
    for (int i = 0; i < inst.n; i++) {
        for (int j = i + 1; j < inst.n; j++) {
            if (inst.related(i, j) || !inst.need_pair(i, j, M[i], M[j])) continue;
            bool ok1 = S[j] >= S[i] + inst.lag(i, j, M[i], M[j]);
            bool ok2 = S[i] >= S[j] + inst.lag(j, i, M[j], M[i]);
            if (!(ok1 || ok2))
                r.errors.push_back("Chong lap giua " + inst.op_names[i] + " va " + inst.op_names[j]);
        }
    }
    long long cmax = 0;
    for (int i = 0; i < inst.n; i++) cmax = std::max(cmax, S[i] + inst.p[i].at(M[i]));
    r.cmax = cmax;
    r.ok = r.errors.empty();
    return r;
}

// ----------------------------------------------------------------------------
// Heuristic tham lam de tim upper bound ban dau
// ----------------------------------------------------------------------------
struct GreedyBest {
    bool ok = false;
    long long cmax = 0;
    std::vector<long long> S;
    std::vector<std::string> M;
};

inline void greedy_schedule_once(const Instance& inst, std::mt19937& rng,
                                  std::vector<long long>& S, std::vector<std::string>& M) {
    int n = inst.n;
    S.assign(n, -1);
    M.assign(n, "");
    std::vector<int> remaining_pred(n);
    for (int i = 0; i < n; i++) remaining_pred[i] = (int) inst.pred[i].size();
    std::vector<int> avail;
    for (int i = 0; i < n; i++) if (remaining_pred[i] == 0) avail.push_back(i);
    std::vector<int> scheduled;
    scheduled.reserve(n);

    while (!avail.empty()) {
        std::uniform_int_distribution<size_t> dist(0, avail.size() - 1);
        size_t pos = dist(rng);
        int i = avail[pos];
        avail[pos] = avail.back();
        avail.pop_back();

        std::vector<std::string> ms = inst.machines[i];
        std::shuffle(ms.begin(), ms.end(), rng);

        long long bestC = -1, bestS = -1;
        std::string bestK;
        for (auto& k : ms) {
            long long s = inst.release.at(k);
            for (int h : inst.pred[i]) s = std::max(s, S[h] + inst.lag(h, i, M[h], k));
            for (int j : scheduled) {
                if (inst.related(i, j)) continue;
                if (inst.need_pair(j, i, M[j], k)) s = std::max(s, S[j] + inst.lag(j, i, M[j], k));
            }
            long long c = s + inst.p[i].at(k);
            if (bestC < 0 || c < bestC) { bestC = c; bestS = s; bestK = k; }
        }
        S[i] = bestS; M[i] = bestK;
        scheduled.push_back(i);
        for (int v : inst.succ[i]) if (--remaining_pred[v] == 0) avail.push_back(v);
    }
}

inline GreedyBest find_upper_bound(const Instance& inst, unsigned seed, double budget_s) {
    std::mt19937 rng(seed);
    GreedyBest best;
    auto t0 = std::chrono::steady_clock::now();
    int it = 0;
    std::vector<long long> S;
    std::vector<std::string> M;
    while (it < 50 ||
           (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < budget_s && it < 20000)) {
        it++;
        greedy_schedule_once(inst, rng, S, M);
        VerifyResult vr = verify(inst, S, M);
        if (vr.ok && (!best.ok || vr.cmax < best.cmax)) {
            best.ok = true; best.cmax = vr.cmax; best.S = S; best.M = M;
        }
    }
    return best;
}

// ----------------------------------------------------------------------------
// Lower bound (giong lower_bound() trong bisec.py, ke ca liet ke tap con may)
// ----------------------------------------------------------------------------
struct Windows { std::vector<long long> ES, LS; };

inline Windows compute_windows(const Instance& inst, long long UB) {
    int n = inst.n;
    std::vector<long long> rmin(n);
    for (int i = 0; i < n; i++) {
        long long best = LLONG_MAX;
        for (auto& k : inst.machines[i]) best = std::min(best, inst.release.at(k));
        rmin[i] = best;
    }
    std::vector<long long> ES(n, 0), LS(n, 0);
    for (int i : inst.topo) {
        long long e = rmin[i];
        for (int h : inst.pred[i]) e = std::max(e, ES[h] + inst.min_edge_lag(h, i));
        ES[i] = e;
    }
    for (auto it = inst.topo.rbegin(); it != inst.topo.rend(); ++it) {
        int i = *it;
        long long minproc = LLONG_MAX;
        for (auto& kv : inst.p[i]) minproc = std::min(minproc, kv.second);
        long long l = UB - minproc;
        for (int j : inst.succ[i]) l = std::min(l, LS[j] - inst.min_edge_lag(i, j));
        LS[i] = l;
    }
    return {ES, LS};
}

inline long long lower_bound(const Instance& inst) {
    Windows w = compute_windows(inst, (long long) 1e9);
    long long bound = 0;
    std::vector<long long> minp(inst.n);
    for (int i = 0; i < inst.n; i++) {
        long long m = LLONG_MAX;
        for (auto& kv : inst.p[i]) m = std::min(m, kv.second);
        minp[i] = m;
        bound = std::max(bound, w.ES[i] + m);
    }
    for (int i = 0; i < inst.n; i++) {
        long long best = LLONG_MAX;
        for (auto& k : inst.machines[i]) best = std::min(best, inst.release.at(k) + inst.p[i].at(k));
        bound = std::max(bound, best);
    }
    long long total_work = 0;
    for (int i = 0; i < inst.n; i++) total_work += minp[i];
    long long machine_count = (long long) inst.all_machines.size();
    if (machine_count > 0) bound = std::max(bound, (total_work + machine_count - 1) / machine_count);

    std::vector<long long> masks;
    if (machine_count <= 12) {
        for (long long mask = 1; mask < (1LL << machine_count); mask++) masks.push_back(mask);
    } else {
        for (int idx = 0; idx < machine_count; idx++) masks.push_back(1LL << idx);
        masks.push_back((1LL << machine_count) - 1);
    }
    for (long long mask : masks) {
        std::unordered_set<std::string> subset;
        for (int idx = 0; idx < machine_count; idx++)
            if (mask & (1LL << idx)) subset.insert(inst.all_machines[idx]);
        long long workload = 0;
        long long subset_size = (long long) subset.size();
        for (int i = 0; i < inst.n; i++) {
            bool sub = true;
            for (auto& k : inst.machines[i]) if (!subset.count(k)) { sub = false; break; }
            if (sub) workload += minp[i];
        }
        if (workload > 0) bound = std::max(bound, (workload + subset_size - 1) / subset_size);
    }
    return bound;
}
