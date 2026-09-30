// cadical_worker.hpp
// Ket noi voi thu vien CaDiCaL (rel-1.9.5, https://github.com/arminbiere/cadical).
// Moi "bound" (gia tri UB dang thu) duoc giai trong mot Solver CaDiCaL rieng,
// chay trong mot process rieng, co the dung qua Terminator hoac SIGTERM.
#pragma once
#include "cadical.hpp" // header cua CaDiCaL (build/ hoac src/), xem BUILD.md
#include "instance.hpp"
#include "cnf_encoder.hpp"
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

// CaDiCaL goi terminate() dinh ky trong luc solve(); tra ve true se lam solve()
// dung som va tra ve 0 (UNKNOWN). Dung de mo phong "timeout" va "huy vi bound
// da bi cac ket qua khac vuot qua" (tuong duong voi _stop_process trong Python,
// nhung la huy hop tac thay vi kill tien trinh vi CaDiCaL chay trong cung tien trinh).
class TimeTerminator : public CaDiCaL::Terminator
{
public:
    TimeTerminator(std::chrono::steady_clock::time_point deadline,
                   std::shared_ptr<std::atomic<bool>> cancel)
        : deadline_(deadline), cancel_(std::move(cancel)) {}

    bool terminate() override
    {
        if (cancel_ && cancel_->load(std::memory_order_relaxed))
            return true;
        return std::chrono::steady_clock::now() >= deadline_;
    }

private:
    std::chrono::steady_clock::time_point deadline_;
    std::shared_ptr<std::atomic<bool>> cancel_;
};

inline void add_clause_to_solver(CaDiCaL::Solver &s, const std::vector<int> &c)
{
    for (int l : c)
        s.add(l);
    s.add(0);
}

struct BoundResult
{
    std::string status;     // "SAT" | "UNSAT" | "TIMEOUT"
    std::vector<int> model; // chi co gia tri khi status == "SAT" (cac bien duong)
};

class IncrementalBoundSolver
{
public:
    IncrementalBoundSolver(const Instance &inst, const CnfModel &model)
        : solver_(), strongest_bound_(LLONG_MAX)
    {
        solver_.set("quiet", 1);
        for (const auto &clause : model.enc.clauses)
            add_clause_to_solver(solver_, clause);
        inst_ = &inst;
        model_ = &model;
    }

    BoundResult solve(long long bound,
                      std::chrono::steady_clock::time_point deadline,
                      std::shared_ptr<std::atomic<bool>> cancel)
    {
        if (bound >= strongest_bound_)
        {
            BoundResult result;
            result.status = "INVALID";
            return result;
        }

        for (const auto &clause : cmax_bound_clauses(*inst_, *model_, bound))
            add_clause_to_solver(solver_, clause);
        strongest_bound_ = bound;

        TimeTerminator terminator(deadline, cancel);
        solver_.connect_terminator(&terminator);
        int status = solver_.solve();
        solver_.disconnect_terminator();

        BoundResult result;
        if (status == 10)
        {
            result.status = "SAT";
            int max_variable = solver_.vars();
            result.model.reserve(max_variable);
            for (int variable = 1; variable <= max_variable; variable++)
                if (solver_.val(variable) > 0)
                    result.model.push_back(variable);
        }
        else if (status == 20)
        {
            result.status = "UNSAT";
        }
        else
        {
            result.status = "TIMEOUT";
        }
        return result;
    }

    long long strongest_bound() const { return strongest_bound_; }

private:
    CaDiCaL::Solver solver_;
    const Instance *inst_ = nullptr;
    const CnfModel *model_ = nullptr;
    long long strongest_bound_;
};
