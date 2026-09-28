#define main fcbqp_embedded_cli_main
#include "../main.cpp"
#undef main

#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void close_to(double actual, double expected, const std::string& message) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > 1e-9) {
        throw std::runtime_error(message + ": got " + std::to_string(actual) +
                                 ", expected " + std::to_string(expected));
    }
}

using Matrix = std::vector<std::vector<double>>;

Instance make_instance(const Matrix& q, int k, bool sparse, double initial_offset) {
    const int n = static_cast<int>(q.size());
    Instance ins;
    ins.n = n;
    ins.k = k;
    ins.sparse = sparse;
    ins.objective_offset = initial_offset;
    ins.diagonal.assign(n, 0.0);
    ins.nnz_full = 0;
    if (sparse) {
        ins.row_ptr.push_back(0);
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < n; ++j) {
                if (q[i][j] == 0.0) continue;
                ins.col_ind.push_back(j); 
                ins.val.push_back(q[i][j]);
                ++ins.nnz_full;
            }
            ins.row_ptr.push_back(static_cast<int>(ins.col_ind.size()));
            ins.diagonal[i] = q[i][i];
        }
    } else {
        ins.Q.reserve(static_cast<size_t>(n) * static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < n; ++j) {
                ins.Q.push_back(q[i][j]);
                ins.nnz_full += q[i][j] != 0.0;
            }
            ins.diagonal[i] = q[i][i];
        }
    }
    ins.density = static_cast<double>(ins.nnz_full) / (n * n);
    return ins;
}

double reference_objective(const Matrix& q, const std::vector<int>& x,
                           double offset) {
    double result = offset;
    for (size_t i = 0; i < q.size(); ++i)
        for (size_t j = 0; j < q.size(); ++j)
            result += q[i][j] * x[i] * x[j];
    return result;
}

void check_csr(const Instance& ins) {
    if (!ins.sparse) return;
    require(ins.row_ptr.size() == static_cast<size_t>(ins.n + 1), "CSR row pointer size");
    require(ins.row_ptr.front() == 0, "CSR first pointer");
    require(ins.row_ptr.back() == static_cast<int>(ins.val.size()), "CSR last pointer");
    require(ins.val.size() == ins.col_ind.size(), "CSR column/value lengths");
    require(ins.val.size() == ins.nnz_full, "CSR nonzero count");
    for (int i = 0; i < ins.n; ++i) {
        int previous = -1;
        for (int p = ins.row_ptr[i]; p < ins.row_ptr[i + 1]; ++p) {
            const int col = ins.col_ind[p];
            require(col > previous && col < ins.n, "CSR columns must be sorted and unique");
            require(ins.val[p] != 0.0, "CSR should not store zero values");
            previous = col;
        }
    }
}

void check_one(const Matrix& q, int k, bool sparse, double initial_offset) {
    const int n = static_cast<int>(q.size());
    Instance original = make_instance(q, k, sparse, initial_offset);
    Instance transformed = original;
    invert_instance(transformed);
    check_csr(transformed);
    require(transformed.k == n - k, "Incorrect complemented cardinality");

    double total_sum = 0.0;
    for (int i = 0; i < n; ++i) {
        double row_sum = 0.0;
        for (int j = 0; j < n; ++j) row_sum += q[i][j];
        total_sum += row_sum;
        close_to(transformed.diagonal[i], q[i][i] - 2.0 * row_sum,
                 "Incorrect transformed diagonal");
        for (int j = 0; j < n; ++j) {
            const double expected = (i == j) ? q[i][i] - 2.0 * row_sum : q[i][j];
            close_to(Qat(transformed, i, j), expected,
                     "Incorrect transformed matrix entry");
        }
    }
    close_to(transformed.objective_offset, initial_offset + total_sum,
             "Incorrect objective offset");

    auto check_mask = [&](const std::vector<int>& x) {
        std::vector<int> y(n);
        for (int i = 0; i < n; ++i) y[i] = 1 - x[i];
        const double expected = reference_objective(q, x, initial_offset);
        const double direct = original.objective_offset + objective_x(x, original);
        const double inverted = transformed.objective_offset + objective_x(y, transformed);
        close_to(direct, expected, "Original objective disagrees with independent evaluation");
        close_to(inverted, expected, "Complemented objective identity failed");
    };

    if (n <= 9) {
        for (int mask = 0; mask < (1 << n); ++mask) {
            std::vector<int> x(n);
            for (int i = 0; i < n; ++i) x[i] = (mask >> i) & 1;
            check_mask(x);
        }
    } else {
        std::vector<int> x(n, 0);
        check_mask(x);
        std::fill(x.begin(), x.end(), 1);
        check_mask(x);
        for (int i = 0; i < n; ++i) {
            std::fill(x.begin(), x.end(), 0);
            x[i] = 1;
            check_mask(x);
        }
        DeterministicRng rng(0x4653425150ULL);
        for (int trial = 0; trial < 64; ++trial) {
            for (int& xi : x) xi = static_cast<int>(bounded_random(rng, 2));
            check_mask(x);
        }
    }

    invert_instance(transformed);
    check_csr(transformed);
    require(transformed.k == k, "Double complement did not recover cardinality");
    close_to(transformed.objective_offset, initial_offset,
             "Double complement did not recover offset");
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
            close_to(Qat(transformed, i, j), q[i][j],
                     "Double complement did not recover matrix");

    Instance canonical = original;
    const bool did_invert = canonicalize_instance(canonical);
    require(did_invert == (k > n / 2 && k > n - k), "Unexpected canonical inversion");
    require(canonical.k == std::min(k, n - k), "Canonical cardinality must be <= n/2");
    if (!did_invert) {
        close_to(canonical.objective_offset, initial_offset,
                 "Unnecessary canonical offset change");
    }
}

Matrix random_symmetric_matrix(int n, uint64_t seed, bool sparse) {
    Matrix q(n, std::vector<double>(n, 0.0));
    DeterministicRng rng(seed);
    for (int i = 0; i < n; ++i) {
        for (int j = i; j < n; ++j) {
            if (sparse && bounded_random(rng, 90) != 0) continue;
            const double value = static_cast<double>(static_cast<int>(bounded_random(rng, 15)) - 7);
            q[i][j] = q[j][i] = value;
        }
    }
    return q;
}

} 

int main() {
    try {
        size_t fixtures = 0;
        for (int n = 2; n <= 8; ++n) {
            for (bool sparse : {false, true}) {
                for (uint64_t seed = 1; seed <= 3; ++seed) {
                    const Matrix q = random_symmetric_matrix(n, seed + 37 * n, sparse);
                    for (int k = 0; k <= n; ++k) {
                        check_one(q, k, sparse, 13.0);
                        ++fixtures;
                    }
                }
            }
        }
        Matrix offdiag(6, std::vector<double>(6, 0.0));
        offdiag[0][4] = offdiag[4][0] = -3.0;
        offdiag[1][2] = offdiag[2][1] = 2.0;
        offdiag[3][3] = 4.0;
        offdiag[5][5] = -4.0;
        for (int k = 0; k <= 6; ++k) {
            check_one(offdiag, k, true, -5.0);
            ++fixtures;
        }
        const Matrix large = random_symmetric_matrix(128, 123456, true);
        for (int k : {0, 1, 2, 63, 64, 65, 126, 127, 128}) {
            check_one(large, k, true, -17.0);
            ++fixtures;
        }
        std::cout << "PASS: " << fixtures
                  << " complement fixtures; exhaustive vectors for n<=8, sampled n=128; "
                  << "dense/CSR, boundary k, offsets, CSR invariants, and double complement\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL complement identity: " << e.what() << '\n';
        return 1;
    }
}
