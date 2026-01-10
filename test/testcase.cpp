// g++ -O3 -march=native -std=c++17 pro_rata_mm.cpp -lpthread
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <vector>
#include <pthread.h>
#include <sched.h>

#include "Latte.hpp"


/*
 * Made from "Optimal high-frequency trading in a pro-rata microstructure with predictive information" (GUILBAUD&PHAM) [https://arxiv.org/pdf/1205.3051]
 * It is NOT a reproduction, just a toy-inspiration for Latte test
 * In order to keep Human-intelligence > AI-slop; do NOT use this toy-reproduction to invest.
 */


// ============================================================
// CONFIG
// ============================================================
static constexpr int    SIM_STEPS = 5'000; // simulation horizon in ticks (reduce for fast compute)
static constexpr double DT_SECONDS = 0.02; // 20ms decision loop
static constexpr double TICK_SIZE = 0.01; // price grid
static constexpr double HALF_SPREAD = TICK_SIZE * 0.5; // one-tick spread assumption

// Predictive signal z in {-1,0,1} with Markov persistence
static constexpr int Z_STATES = 3; // idx: 0->-1, 1->0, 2->+1

// Inventory grid (DP state space)
static constexpr int Y_MAX = 40; // clamp inventory to [-Y_MAX, Y_MAX]
static constexpr int Y_SIZE = 2 * Y_MAX + 1;

// Quote size levels (pro-rata: displayed volume at best bid/ask)
// level 0 means "no quote"
static constexpr int Q_LEVELS = 4;
static constexpr std::array<int, Q_LEVELS> Q_LEVEL = {0, 2'000, 8'000, 20'000};

// Others' depth at best (constant for simplicity; can be random if you want)
static constexpr int DEPTH_OTHERS = 200'000;

// Market order size distribution (discrete pmf over {1..M_MAX})
static constexpr int M_MAX = 200;
static constexpr double M_MEAN = 40.0; // shape param for pmf

// Fill volume cap for DP expectation truncation (keep small for speed)
static constexpr int V_MAX = 25;

// Order arrival base rates (per second). Signal creates imbalance between sides.
static constexpr double LAMBDA_BASE = 12.0;   // total activity
static constexpr double LAMBDA_IMB  = 0.35;   // imbalance sensitivity to signal in [-1,1]

// Midprice model (Bachelier with signal-dependent drift, rounded to tick)
static constexpr double SIGMA = 0.03;     // price units / sqrt(second)
static constexpr double MU0   = 0.002;    // drift amplitude (price units / second) when z=+1

// Risk penalty (mean-variance style): -gamma * sigma^2 * ∫ y^2 dt
static constexpr double GAMMA_RISK = 5e-4;

// Take (market order) fees/costs in addition to crossing half-spread
static constexpr double FEE_TAKE_PER_SHARE = 0.0;
static constexpr double FEE_TAKE_FIXED     = 0.0;

// =================================================
// Helpers
// =================================================
static inline int z_idx_to_val(int zi) {
    // 0->-1, 1->0, 2->+1
    return zi - 1;
}

static inline int clamp_y(int y) {
    return std::max(-Y_MAX, std::min(Y_MAX, y));
}

static inline int y_to_idx(int y) {
    return clamp_y(y) + Y_MAX;
}

static inline double round_to_tick(double px) {
    return std::round(px / TICK_SIZE) * TICK_SIZE;
}

// Discrete market order size pmf over m=1..M_MAX
static std::array<double, M_MAX + 1> build_m_pmf() {
    std::array<double, M_MAX + 1> pmf{};
    pmf.fill(0.0);
    double Z = 0.0;
    for (int m = 1; m <= M_MAX; ++m) {
        // exponential tail (simple dummy)
        double w = std::exp(-static_cast<double>(m) / M_MEAN);
        pmf[m] = w;
        Z += w;
    }
    for (int m = 1; m <= M_MAX; ++m) pmf[m] /= Z;
    return pmf;
}

static inline int pro_rata_fill(int M, int Q, int D_others) {
    if (Q <= 0) return 0;
    const double share = static_cast<double>(Q) / static_cast<double>(Q + D_others);
    int v = static_cast<int>(std::floor(share * static_cast<double>(M)));
    if (v < 0) v = 0;
    return std::min(v, V_MAX);
}

// Precompute fill pmf over v=0..V_MAX for each quote size level, conditional on "an arrival occurs".
static std::array<std::array<double, V_MAX + 1>, Q_LEVELS>
build_fill_pmf_given_arrival(const std::array<double, M_MAX + 1>& m_pmf) {
    std::array<std::array<double, V_MAX + 1>, Q_LEVELS> out{};
    for (int ql = 0; ql < Q_LEVELS; ++ql) {
        out[ql].fill(0.0);
        const int Q = Q_LEVEL[ql];
        for (int m = 1; m <= M_MAX; ++m) {
            const int v = pro_rata_fill(m, Q, DEPTH_OTHERS);
            out[ql][v] += m_pmf[m];
        }
        // already sums to 1
    }
    return out;
}

// Sample from discrete pmf on m=1..M_MAX
struct DiscreteSampler {
    std::vector<double> cdf; // size M_MAX+1
    explicit DiscreteSampler(const std::array<double, M_MAX + 1>& pmf) {
        cdf.resize(M_MAX + 1, 0.0);
        double run = 0.0;
        for (int m = 1; m <= M_MAX; ++m) {
            run += pmf[m];
            cdf[m] = run;
        }
        // Ensure last is 1
        cdf[M_MAX] = 1.0;
    }
    int sample(std::mt19937_64& rng) const {
        std::uniform_real_distribution<double> U(0.0, 1.0);
        double u = U(rng);
        int lo = 1, hi = M_MAX;
        while (lo < hi) {
            int mid = (lo + hi) >> 1;
            if (u <= cdf[mid]) hi = mid;
            else lo = mid + 1;
        }
        return lo;
    }
};

// ==========================================================================
// DP Policy (discrete approximation of the control problem)
// State: (t, y, z)
// Control:
//   - make: choose bid quote level qb_idx in {0, ..., Q_LEVELS-1}, ask qa_idx
//   - take: impulse in {NONE, FLATTEN, BUY1, SELL1, BUY2, SELL2}
//============================================================================
enum class Impulse : int8_t { NONE=0, FLATTEN=1, BUY1=2, SELL1=3, BUY2=4, SELL2=5 };

struct Action {
    int8_t qb; // quote level index at bid
    int8_t qa; // quote level index at ask
    Impulse imp; // market order impulse
};

struct Params {
    double dt = DT_SECONDS;
    double tick = TICK_SIZE;
    double half_spread = HALF_SPREAD;
    double sigma = SIGMA;
    double mu0 = MU0;
    double gamma_risk = GAMMA_RISK;
    double fee_take_per_share = FEE_TAKE_PER_SHARE;
    double fee_take_fixed = FEE_TAKE_FIXED;

    std::array<std::array<double, Z_STATES>, Z_STATES> Pz{{
        // z=-1,0,+1 rows; fairly persistent
        std::array<double,3>{0.92, 0.07, 0.01},
        std::array<double,3>{0.05, 0.90, 0.05},
        std::array<double,3>{0.01, 0.07, 0.92},
    }};
};

class ProRataMM {
public:
    explicit ProRataMM(const Params& p) : P(p), rng(42), N(SIM_STEPS), V((N+1) * Y_SIZE * Z_STATES, -1e100), Pi(N * Y_SIZE * Z_STATES) {

        Latte::Hard::Start("Init_Precompute");
        m_pmf = build_m_pmf();
        fill_pmf = build_fill_pmf_given_arrival(m_pmf);
        m_sampler = std::make_unique<DiscreteSampler>(m_pmf);
        Latte::Hard::Stop("Init_Precompute");
    }

    void BuildPolicyDP() {
        Latte::Hard::Start("DP_Build_Total");
        // Terminal condition: immediate liquidation via market order (cross half-spread + fees)
        // v_T(y,z) = - (half_spread + fee_per_share)*|y| - fee_fixed * 1_{y != 0}
        for (int zi = 0; zi < Z_STATES; ++zi) {
            for (int y = -Y_MAX; y <= Y_MAX; ++y) {
                double term = - (P.half_spread + P.fee_take_per_share) * std::abs(y);
                if (y != 0) term -= P.fee_take_fixed;
                V[idxV(N, y, zi)] = term;
            }
        }

        // Precompute make-actions (b, a)
        std::vector<std::pair<int,int>> make_actions;
        make_actions.reserve(Q_LEVELS * Q_LEVELS);
        for (int qb = 0; qb < Q_LEVELS; ++qb)
            for (int qa = 0; qa < Q_LEVELS; ++qa)
                make_actions.push_back({qb, qa});

        
        for (int t = N - 1; t >= 0; --t) { // Backward
            Latte::Fast::Start("DP_TimeSlice");

            for (int zi = 0; zi < Z_STATES; ++zi) {
                const int z = z_idx_to_val(zi);
                const double mu = P.mu0 * static_cast<double>(z);

                // Arrival probabilities (0/1 approximation): p = 1-exp(-lambda dt)
                // Ask arrivals are buys hitting our ask (we SELL). Bid-side arrivals are sells hitting our bid (we BUY).
                double lambda_ask = LAMBDA_BASE * (1.0 + LAMBDA_IMB * static_cast<double>(z));
                double lambda_bid = LAMBDA_BASE * (1.0 - LAMBDA_IMB * static_cast<double>(z));
                lambda_ask = std::max(1e-6, lambda_ask);
                lambda_bid = std::max(1e-6, lambda_bid);

                const double p_ask = 1.0 - std::exp(-lambda_ask * P.dt);
                const double p_bid = 1.0 - std::exp(-lambda_bid * P.dt);

                for (int y = -Y_MAX; y <= Y_MAX; ++y) {
                    LATTE_PULSE("DP_StateLoop");

                    // Running term (mean-variance style): y*mu*dt - gamma*sigma^2*y^2*dt
                    const double run = (static_cast<double>(y) * mu) * P.dt
                                     - (P.gamma_risk * (P.sigma * P.sigma) * static_cast<double>(y*y)) * P.dt;

                    double best = -1e100;
                    Action bestA{0,0,Impulse::NONE};

                    std::array<Impulse,6> impulses = {
                        Impulse::NONE, Impulse::FLATTEN,
                        Impulse::BUY1, Impulse::SELL1,
                        Impulse::BUY2, Impulse::SELL2
                    };

                    for (auto imp : impulses) {
                        // Apply impulse inventory change
                        int dy = 0;
                        if (imp == Impulse::FLATTEN) dy = -y;
                        else if (imp == Impulse::BUY1) dy = +1;
                        else if (imp == Impulse::SELL1) dy = -1;
                        else if (imp == Impulse::BUY2) dy = +2;
                        else if (imp == Impulse::SELL2) dy = -2;

                        int y1 = clamp_y(y + dy);

                        // Impulse cost in correction coordinates: -(half_spread+fee)*|dy| - fixed_fee*I(dy!=0)
                        double imp_cost = 0.0;
                        if (dy != 0) {
                            imp_cost -= (P.half_spread + P.fee_take_per_share) * std::abs(dy);
                            imp_cost -= P.fee_take_fixed;
                        }

                        // Make actions (bid/ask displayed volume levels)
                        for (const auto& ma : make_actions) {
                            const int qb = ma.first;
                            const int qa = ma.second;

                            // If quote level is 0 -> not participating on that side
                            const bool on_bid = (Q_LEVEL[qb] > 0);
                            const bool on_ask = (Q_LEVEL[qa] > 0);

                            // Side-specific arrival prob is 0 if not quoting
                            const double pB = on_bid ? p_bid : 0.0;
                            const double pA = on_ask ? p_ask : 0.0;

                            // z transition + independent (vb,va) draws
                            double ev = 0.0;

                            for (int zj = 0; zj < Z_STATES; ++zj) {
                                const double pz = P.Pz[zi][zj];
                                if (pz == 0.0) continue;

                                // vb distribution (bid fills)
                                for (int vb = 0; vb <= V_MAX; ++vb) {
                                    double pvb = 0.0;
                                    if (!on_bid) {
                                        pvb = (vb == 0) ? 1.0 : 0.0;
                                    } else {
                                        // 0 if no arrival + arrival with v=vb
                                        pvb = (1.0 - pB) * (vb == 0 ? 1.0 : 0.0)
                                            + pB * fill_pmf[qb][vb];
                                    }
                                    if (pvb == 0.0) continue;

                                    // va distribution (ask fills)
                                    for (int va = 0; va <= V_MAX; ++va) {
                                        double pva = 0.0;
                                        if (!on_ask) {
                                            pva = (va == 0) ? 1.0 : 0.0;
                                        } else {
                                            pva = (1.0 - pA) * (va == 0 ? 1.0 : 0.0)
                                                + pA * fill_pmf[qa][va];
                                        }
                                        if (pva == 0.0) continue;

                                        const int y2 = clamp_y(y1 + vb - va);

                                        // Limit-order execution gives +half_spread per share (in correction coordinates)
                                        const double make_gain = P.half_spread * static_cast<double>(vb + va);

                                        ev += pz * pvb * pva * (make_gain + V[idxV(t+1, y2, zj)]);
                                    }
                                }
                            }

                            const double val = run + imp_cost + ev;
                            if (val > best) {
                                best = val;
                                bestA = Action{static_cast<int8_t>(qb), static_cast<int8_t>(qa), imp};
                            }
                        }
                    }

                    V[idxV(t, y, zi)] = best;
                    Pi[idxPi(t, y, zi)] = bestA;
                }
            }

            Latte::Fast::Stop("DP_TimeSlice");
        }

        Latte::Hard::Stop("DP_Build_Total");
    }

    // Simulation using the optimal policy computed by DP
    void RunSimulation() {
        Latte::Hard::Start("Sim_Total");

        double S = 100.00; // midprice
        int y = 0;         // inventory
        double cash = 0.0;

        int zi = 1; // start at z=0 state
        std::normal_distribution<double> N01(0.0, 1.0);
        std::uniform_real_distribution<double> U(0.0, 1.0);

        // Poisson distributions updated each tick (since lambda depends on z)
        for (int t = 0; t < N; ++t) {
            Latte::Hard::Start("Sim_Tick_Total");

            // --- Control lookup (clamp inventory into DP grid)
            Latte::Mid::Start("Sim_PolicyLookup");
            const Action a = Pi[idxPi(t, y, zi)];
            Latte::Mid::Stop("Sim_PolicyLookup");

            // --- TAKE (market order impulse)
            Latte::Fast::Start("Sim_Impulse");
            int dy = 0;
            if (a.imp == Impulse::FLATTEN) dy = -y;
            else if (a.imp == Impulse::BUY1) dy = +1;
            else if (a.imp == Impulse::SELL1) dy = -1;
            else if (a.imp == Impulse::BUY2) dy = +2;
            else if (a.imp == Impulse::SELL2) dy = -2;

            if (dy != 0) {
                const double px = (dy > 0) ? (S + HALF_SPREAD) : (S - HALF_SPREAD);
                const int qty = std::abs(dy);
                // cash change with explicit prices + fees
                if (dy > 0) cash -= px * qty;
                else        cash += px * qty;
                cash -= FEE_TAKE_PER_SHARE * qty;
                cash -= FEE_TAKE_FIXED;
                y = clamp_y(y + dy);
            }
            Latte::Fast::Stop("Sim_Impulse");

            // --- MAKE (quotes at best; pro-rata executed volume)
            Latte::Fast::Start("Sim_OrderFlow");

            const int z = z_idx_to_val(zi);
            double lambda_ask = LAMBDA_BASE * (1.0 + LAMBDA_IMB * static_cast<double>(z)); // buys hit ask -> we sell
            double lambda_bid = LAMBDA_BASE * (1.0 - LAMBDA_IMB * static_cast<double>(z)); // sells hit bid -> we buy
            lambda_ask = std::max(1e-6, lambda_ask);
            lambda_bid = std::max(1e-6, lambda_bid);

            std::poisson_distribution<int> PoisAsk(lambda_ask * DT_SECONDS);
            std::poisson_distribution<int> PoisBid(lambda_bid * DT_SECONDS);

            const int qb = a.qb;
            const int qa = a.qa;
            const int Qb = Q_LEVEL[qb];
            const int Qa = Q_LEVEL[qa];

            // Ask-side arrivals
            int nAsk = (Qa > 0) ? PoisAsk(rng) : 0;
            for (int k = 0; k < nAsk; ++k) {
                LATTE_PULSE("Sim_AskLoop");
                const int M = m_sampler->sample(rng);
                const int v = pro_rata_fill(M, Qa, DEPTH_OTHERS);
                if (v > 0) {
                    cash += (S + HALF_SPREAD) * static_cast<double>(v);
                    y = clamp_y(y - v);
                }
            }

            // Bid-side arrivals
            int nBid = (Qb > 0) ? PoisBid(rng) : 0;
            for (int k = 0; k < nBid; ++k) {
                LATTE_PULSE("Sim_BidLoop");
                const int M = m_sampler->sample(rng);
                const int v = pro_rata_fill(M, Qb, DEPTH_OTHERS);
                if (v > 0) {
                    cash -= (S - HALF_SPREAD) * static_cast<double>(v);
                    y = clamp_y(y + v);
                }
            }

            Latte::Fast::Stop("Sim_OrderFlow");

            // --- Midprice evolution with predictive drift + diffusion (rounded to tick)
            Latte::Mid::Start("Sim_PriceEvolve");
            const double mu = MU0 * static_cast<double>(z);
            S += mu * DT_SECONDS + SIGMA * std::sqrt(DT_SECONDS) * N01(rng);
            S = round_to_tick(S);
            Latte::Mid::Stop("Sim_PriceEvolve");

            // --- Signal transition
            Latte::Fast::Start("Sim_SignalEvolve");
            {
                double u = U(rng);
                double c = 0.0;
                int zj = 0;
                for (; zj < Z_STATES; ++zj) {
                    c += P.Pz[zi][zj];
                    if (u <= c) break;
                }
                zi = std::min(zj, Z_STATES - 1);
            }
            Latte::Fast::Stop("Sim_SignalEvolve");

            // --- Risk / PnL bookkeeping
            Latte::Fast::Start("Sim_RiskPnL");
            // (toy) compute MTM each tick (is expensive on purpose of Latte test)
            volatile double mtm = cash + static_cast<double>(y) * S;
            (void)mtm;
            Latte::Fast::Stop("Sim_RiskPnL");

            Latte::Hard::Stop("Sim_Tick_Total");
        }

        // Final liquidation at market
        Latte::Mid::Start("Sim_FinalLiquidation");
        if (y != 0) {
            const double px = (y > 0) ? (S - HALF_SPREAD) : (S + HALF_SPREAD); // sell long at bid, buy back short at ask
            const int qty = std::abs(y);
            if (y > 0) cash += px * qty;
            else       cash -= px * qty;
            cash -= FEE_TAKE_PER_SHARE * qty;
            cash -= FEE_TAKE_FIXED;
            y = 0;
        }
        Latte::Mid::Stop("Sim_FinalLiquidation");

        const double final_mtm = cash; // y==0
        std::cout << "Final MTM (liquidated): " << std::fixed << std::setprecision(6) << final_mtm << "\n";

        Latte::Hard::Stop("Sim_Total");
    }

private:
    Params P;
    std::mt19937_64 rng;
    int N;

    std::array<double, M_MAX + 1> m_pmf{};
    std::array<std::array<double, V_MAX + 1>, Q_LEVELS> fill_pmf{};
    std::unique_ptr<DiscreteSampler> m_sampler;

    // Value function correction v(t,y,z) stored flat
    std::vector<double> V;
    // Policy stored flat
    std::vector<Action> Pi;

    inline size_t idxV(int t, int y, int zi) const {
        const size_t tt = static_cast<size_t>(t);
        const size_t yy = static_cast<size_t>(y_to_idx(y));
        const size_t zz = static_cast<size_t>(zi);
        return (tt * Y_SIZE + yy) * Z_STATES + zz;
    }
    inline size_t idxPi(int t, int y, int zi) const {
        // policy defined for t in [0..N-1]
        const size_t tt = static_cast<size_t>(t);
        const size_t yy = static_cast<size_t>(y_to_idx(y));
        const size_t zz = static_cast<size_t>(zi);
        return (tt * Y_SIZE + yy) * Z_STATES + zz;
    }
};

static void pin_thread_to_core(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) != 0) {
        std::cerr << "[WARN] Failed to pin thread. Latency data may be noisy.\n";
    }
}

int main() {
    pin_thread_to_core(4);

    Params P;

    std::cout << "Building pro-rata HFT/MM policy (Guilbaud-Pham style)...\n";
    std::cout << "SIM_STEPS=" << SIM_STEPS << "  DT=" << DT_SECONDS << "s  Y_MAX=" << Y_MAX << "\n";

    ProRataMM mm(P);
    LATTE_CALIBRATE();
    // Warmup: touch code paths & caches
    std::cout << "Warmup...\n";
    for (volatile int i = 0; i < 50; ++i) {
        LATTE_PULSE("Warmup_Pulse");
    }

    std::cout << "Here it comes.." << std::endl;
    // Build DP policy
    mm.BuildPolicyDP();

    // Run simulation with latency monitoring
    mm.RunSimulation();

    std::cout << "\n>>> LATTE REPORT (cycles) <<<\n";
    Latte::DumpToStream(std::cout, Latte::Parameter::Time, Latte::Parameter::Raw);
    std::cout << "\n\n" << std::endl;
    Latte::DumpToStream(std::cout, Latte::Parameter::Time, Latte::Parameter::Calibrated);
    return 0;
}
