// cnf_encoder.hpp
// Ma hoa CNF theo Order Encoding, dung file "Setup_Time_FJSSP.pdf" (muc 4)
// va tuong ung 1-1 voi build_cnf()/add_cmax_bound()/decode() trong bisec.py.
#pragma once
#include "instance.hpp"
#include <optional>
#include <unordered_set>

// Bien 1 (T) luon TRUE - dung lam hang so cho khoa bien Order Encoding.
struct Encoder
{
    static constexpr int T = 1;
    int nv = 1;
    std::vector<std::vector<int>> clauses;
    bool unsat = false;

    Encoder() { clauses.push_back({T}); }

    int new_var() { return ++nv; }

    void add(std::vector<int> lits)
    {
        std::vector<int> out;
        out.reserve(lits.size());
        for (int l : lits)
        {
            if (l == T)
                return; // menh de luon thoa, bo qua
            if (l == -T)
                continue; // literal luon sai, loai khoi menh de
            out.push_back(l);
        }
        if (out.empty())
            unsat = true;
        clauses.push_back(std::move(out));
    }
};

struct CnfModel
{
    Encoder enc;
    // sv[i][t], xv[i][t] (chi ton tai t trong [ES[i]+1, LS[i]] doi voi xv)
    std::vector<std::unordered_map<long long, int>> sv, xv;
    std::vector<std::unordered_map<std::string, int>> mv;
    std::vector<long long> ES, LS;
};

// X(i,t): bien "i bat dau tai thoi diem >= t", co khoa bien duoi/tren.
inline int X_lit(const CnfModel &m, int i, long long t)
{
    if (t <= m.ES[i])
        return Encoder::T;
    if (t > m.LS[i])
        return -Encoder::T;
    auto it = m.xv[i].find(t);
    return it->second; // phai ton tai neu ES[i] < t <= LS[i]
}

inline std::optional<CnfModel> build_cnf(const Instance &inst, long long UB)
{
    int n = inst.n;
    Windows w = compute_windows(inst, UB);
    for (int i = 0; i < n; i++)
        if (w.LS[i] < w.ES[i])
            return std::nullopt;

    CnfModel m;
    m.ES = w.ES;
    m.LS = w.LS;
    m.sv.resize(n);
    m.xv.resize(n);
    m.mv.resize(n);
    Encoder &e = m.enc;

    // --- Khai bao bien ---
    for (int i = 0; i < n; i++)
    {
        for (long long t = m.ES[i] + 1; t <= m.LS[i]; t++)
            m.xv[i][t] = e.new_var();
        for (long long t = m.ES[i]; t <= m.LS[i]; t++)
            m.sv[i][t] = e.new_var();
        for (auto &k : inst.machines[i])
            m.mv[i][k] = e.new_var();
    }

    auto X = [&](int i, long long t)
    { return X_lit(m, i, t); };

    // --- 4.1 Lien ket bien Order x, bien thoi diem s va khoa bien ---
    for (int i = 0; i < n; i++)
    {
        for (long long t = m.ES[i]; t <= m.LS[i]; t++)
        {
            int s = m.sv[i][t];
            int x0 = X(i, t), x1 = X(i, t + 1);
            e.add({-s, x0});
            e.add({-s, -x1});
            e.add({-x0, x1, s});
            e.add({-x1, x0}); // tinh don dieu x_{i,t+1} -> x_{i,t}
        }
    }

    // --- 4.2 Rang buoc gan may duy nhat (ALO + AMO) + release time ---
    for (int i = 0; i < n; i++)
    {
        std::vector<int> alo;
        alo.reserve(inst.machines[i].size());
        for (auto &k : inst.machines[i])
            alo.push_back(m.mv[i][k]);
        e.add(alo);
        for (size_t a = 0; a < inst.machines[i].size(); a++)
            for (size_t b = a + 1; b < inst.machines[i].size(); b++)
                e.add({-m.mv[i][inst.machines[i][a]], -m.mv[i][inst.machines[i][b]]});
        for (auto &k : inst.machines[i])
        {
            long long rel = inst.release.at(k);
            if (rel > 0)
                e.add({-m.mv[i][k], X(i, rel)});
        }
    }

    // --- 4.3 Precedence theo DAG (transport neu khac may, setup neu cung may) ---
    for (int i = 0; i < n; i++)
    {
        for (int j : inst.succ[i])
        {
            for (auto &k1 : inst.machines[i])
            {
                for (auto &k2 : inst.machines[j])
                {
                    long long lg = inst.lag(i, j, k1, k2);
                    int mi = m.mv[i][k1], mj = m.mv[j][k2];
                    for (long long t = m.ES[i]; t <= m.LS[i]; t++)
                        e.add({-m.sv[i][t], -mi, -mj, X(j, t + lg)});
                }
            }
        }
    }

    // --- 4.4 + 4.5 Khong chong lap / setup cung may / van chuyen lien may (assembly) ---
    for (int i = 0; i < n; i++)
    {
        for (int j = i + 1; j < n; j++)
        {
            if (inst.related(i, j))
                continue; // thu tu da duoc DAG (ke ca bac cau) quyet dinh
            for (auto &k1 : inst.machines[i])
            {
                for (auto &k2 : inst.machines[j])
                {
                    if (!inst.need_pair(i, j, k1, k2))
                        continue;
                    int mi = m.mv[i][k1], mj = m.mv[j][k2];
                    long long l_ij = inst.lag(i, j, k1, k2);
                    long long l_ji = inst.lag(j, i, k2, k1);
                    int a = e.new_var(); // a <-> (m_i,k1 ^ m_j,k2)
                    e.add({-mi, -mj, a});
                    e.add({-a, mi});
                    e.add({-a, mj});
                    // a ^ s_{i,t} -> (j bat dau >= t+l_ij) v (j bat dau <= t-l_ji)
                    for (long long t = m.ES[i]; t <= m.LS[i]; t++)
                        e.add({-a, -m.sv[i][t], X(j, t + l_ij), -X(j, t - l_ji + 1)});
                }
            }
        }
    }

    if (e.unsat)
        return std::nullopt;
    return m;
}

// --- 4.6 Cmax <= UB, chi ap dung cho cac operation cuoi cua DAG (khong co successor) ---
// Tra ve danh sach menh de tho (khong qua Encoder::add) de them truc tiep vao 1 Solver rieng.
inline std::vector<std::vector<int>> cmax_bound_clauses(const Instance &inst, const CnfModel &m, long long UB)
{
    std::vector<std::vector<int>> out;
    for (int i = 0; i < inst.n; i++)
    {
        if (inst.succ[i].empty())
        {
            for (auto &k : inst.machines[i])
            {
                long long need = UB - inst.p[i].at(k) + 1;
                int lit = X_lit(m, i, need);
                // X(i, t) means S_i >= t, so Cmax <= UB requires S_i < need.
                out.push_back({-m.mv[i].at(k), -lit});
            }
        }
    }
    return out;
}

struct Schedule
{
    std::vector<long long> S;
    std::vector<std::string> M;
};

// model_true: danh sach cac bien duong (var id) dung trong mo hinh SAT.
inline Schedule decode(const Instance &inst, const std::vector<int> &model_true, const CnfModel &m)
{
    std::unordered_set<int> trueset(model_true.begin(), model_true.end());
    Schedule sch;
    sch.S.assign(inst.n, -1);
    sch.M.assign(inst.n, "");
    for (int i = 0; i < inst.n; i++)
    {
        for (long long t = m.ES[i]; t <= m.LS[i]; t++)
        {
            auto it = m.sv[i].find(t);
            if (it != m.sv[i].end() && trueset.count(it->second))
            {
                sch.S[i] = t;
                break;
            }
        }
        for (auto &k : inst.machines[i])
        {
            auto it = m.mv[i].find(k);
            if (it != m.mv[i].end() && trueset.count(it->second))
            {
                sch.M[i] = k;
                break;
            }
        }
    }
    return sch;
}
