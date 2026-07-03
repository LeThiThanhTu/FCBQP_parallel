#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <cctype>
#include <cstdlib>
#include <omp.h>
#include <filesystem>
#include <atomic>
#include <unordered_map>

namespace fs = std::filesystem;

struct Instance {
   int n = 0;
   int k = 0;
   bool sparse = false;
   double density = 0.0;
   size_t nnz_full = 0;
   std::vector<double> Q;       
   std::vector<int> row_ptr;    
   std::vector<int> col_ind;    
   std::vector<double> val;     
};

static constexpr double SPARSE_DENSITY_THRESHOLD = 0.002; 

static inline double Qat(const Instance& ins, int i, int j) {
   if (!ins.sparse) {
      return ins.Q[static_cast<size_t>(i) * static_cast<size_t>(ins.n) + static_cast<size_t>(j)];
   }
   const int begin = ins.row_ptr[i];
   const int end = ins.row_ptr[i + 1];
   auto it = std::lower_bound(ins.col_ind.begin() + begin, ins.col_ind.begin() + end, j);
   if (it == ins.col_ind.begin() + end || *it != j) return 0.0;
   const size_t pos = static_cast<size_t>(it - ins.col_ind.begin());
   return ins.val[pos];
}

static inline double& Qref(Instance& ins, int i, int j) {
   if (ins.sparse) {
      throw std::runtime_error("Qref cannot be used with sparse CSR storage.");
   }
   return ins.Q[static_cast<size_t>(i) * static_cast<size_t>(ins.n) + static_cast<size_t>(j)];
}

struct Params {
   int num_starts = 30;
   double v_0_frac = 0.40;
   double balance = 0.67;
   int T_max = 5000;
   int t_alpha = 15;
   int v_1 = 40;
   int v_2 = 40;
   double d_frac = 0.2;
   double time_limit_seconds = 600.0;
};

struct Result {
   std::vector<int> best_x;
   double best_obj = std::numeric_limits<double>::infinity();
   double time_to_best = 0.0;
};

struct EliteSol {
   std::vector<int> x;
   double obj = std::numeric_limits<double>::infinity();
};

static inline std::string trim(const std::string& s) {
   const size_t a = s.find_first_not_of(" \t\r\n");
   if (a == std::string::npos) return "";
   const size_t b = s.find_last_not_of(" \t\r\n");
   return s.substr(a, b - a + 1);
}

static inline bool starts_with_hash(const std::string& s) {
   std::string t = trim(s);
   return !t.empty() && t[0] == '#';
}

static inline bool time_limit_reached(
   const std::chrono::steady_clock::time_point& t_start,
   double time_limit_seconds
) {
   if (time_limit_seconds <= 0.0) return false;
   const auto t_now = std::chrono::steady_clock::now();
   const double elapsed = std::chrono::duration<double>(t_now - t_start).count();
   return elapsed >= time_limit_seconds;
}

static std::string basename_without_extension(const std::string& path) {
   size_t slash = path.find_last_of("/\\");
   std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
   size_t dot = name.find_last_of('.');
   if (dot != std::string::npos) name = name.substr(0, dot);
   return name;
}

static bool regular_file_exists(const std::string& path) {
   std::error_code ec;
   return fs::exists(path, ec) && fs::is_regular_file(path, ec);
}

static bool directory_exists(const std::string& path) {
   std::error_code ec;
   return fs::exists(path, ec) && fs::is_directory(path, ec);
}

static std::string join_path(const std::string& dir, const std::string& file) {
   if (dir.empty()) return file;
   const char last = dir.back();
   if (last == '/' || last == '\\') return dir + file;
   return dir + "/" + file;
}

enum class ObjectiveSense {
   Min,
   Max
};

enum class PrintScale {
   Full,
   Half
};

ObjectiveSense parse_objective_sense(const std::string& s) {
   std::string t = s;
   std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
      });

   if (t == "min") return ObjectiveSense::Min;
   if (t == "max") return ObjectiveSense::Max;

   throw std::runtime_error("Objective must be either 'max' or 'min'.");
}

PrintScale parse_print_scale(const std::string& s) {
   std::string t = s;
   std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
      });

   if (t == "full") return PrintScale::Full;
   if (t == "half") return PrintScale::Half;

   throw std::runtime_error("Scaling must be either 'full' or 'half'.");
}

static inline const char* objective_sense_name(ObjectiveSense sense) {
   return (sense == ObjectiveSense::Max) ? "max" : "min";
}

static inline const char* print_scale_name(PrintScale scale) {
   return (scale == PrintScale::Full) ? "full" : "half";
}

static inline double output_objective_from_internal(double internal_obj, ObjectiveSense sense, PrintScale scale) {
   if (sense == ObjectiveSense::Max) {
      return (scale == PrintScale::Full)
         ? (-1.0) * internal_obj
         : (-0.5) * internal_obj;
   }

   return (scale == PrintScale::Full)
      ? internal_obj
      : 0.5 * internal_obj;
}

static inline bool target_reached(double output_obj, double target_obj, ObjectiveSense sense, double tol = 1e-6) {
   if (!std::isfinite(target_obj)) return false;

   if (sense == ObjectiveSense::Max) {
      return output_obj >= target_obj - tol;
   }

   return output_obj <= target_obj + tol;
}

struct BestKnownEntry {
   bool loaded = false;
   std::string instance;
   std::string matched_instance;
   std::string filename;
   double objective = std::numeric_limits<double>::quiet_NaN();
};

static std::vector<std::string> parse_csv_line(const std::string& line) {
   std::vector<std::string> fields;
   std::string cur;
   bool in_quotes = false;
   for (size_t i = 0; i < line.size(); ++i) {
      const char c = line[i];

      if (c == '"') {
         if (in_quotes && i + 1 < line.size() && line[i + 1] == '"') {
            cur.push_back('"');
            ++i;
         }
         else {
            in_quotes = !in_quotes;
         }
      }
      else if (c == ',' && !in_quotes) {
         fields.push_back(trim(cur));
         cur.clear();
      }
      else {
         cur.push_back(c);
      }
   }
   fields.push_back(trim(cur));
   return fields;
}

static std::unordered_map<std::string, BestKnownEntry> read_best_known_csv(const std::string& filename) {
   std::ifstream in(filename);
   if (!in) {
      throw std::runtime_error("Cannot open best-known CSV file: " + filename);
   }
   std::unordered_map<std::string, BestKnownEntry> table;
   std::string line;
   if (!std::getline(in, line)) {
      throw std::runtime_error("Best-known CSV file is empty: " + filename);
   }
   int line_no = 1;
   while (std::getline(in, line)) {
      ++line_no;
      std::string t = trim(line);
      if (t.empty()) continue;
      std::vector<std::string> f = parse_csv_line(t);
      if (f.size() < 2) {
         std::cerr << "WARNING: skipping malformed CSV line " << line_no << "\n";
         continue;
      }
      const std::string instance = trim(f[0]);
      const std::string obj_str = trim(f[1]);

      if (instance.empty() || obj_str.empty()) {
         std::cerr << "WARNING: skipping CSV line " << line_no
            << " because instance or best_known_obj is empty.\n";
         continue;
      }
      char* endptr = nullptr;
      const double best_obj = std::strtod(obj_str.c_str(), &endptr);
      if (endptr == obj_str.c_str()) {
         std::cerr << "WARNING: skipping CSV line " << line_no
            << " because best_known_obj is not numeric: "
            << obj_str << "\n";
         continue;
      }
      BestKnownEntry e;
      e.loaded = true;
      e.instance = instance;
      e.matched_instance = instance;
      e.objective = best_obj;
      table[instance] = e;
   }
   return table;
}

static BestKnownEntry find_best_known_for_instance(const std::string& csv_filename, const std::string& instance_path) {
   if (csv_filename.empty()) {
      return BestKnownEntry{};
   }
   const auto table = read_best_known_csv(csv_filename);
   const std::string base = basename_without_extension(instance_path);
   auto it = table.find(base);
   if (it != table.end()) return it->second;
   const std::string name_with_ext = fs::path(instance_path).filename().string();
   it = table.find(name_with_ext);
   if (it != table.end()) return it->second;
   return BestKnownEntry{};
}

using BestKnownObjective = BestKnownEntry;

static std::string resolve_best_known_objective_path(const std::string& instance_path, const std::string& best_known_arg) {
   (void)instance_path;

   if (best_known_arg.empty()) return "";

   if (regular_file_exists(best_known_arg)) {
      return best_known_arg;
   }

   if (directory_exists(best_known_arg)) {
      const std::string csv_path = join_path(best_known_arg, "best_known_results.csv");
      if (regular_file_exists(csv_path)) return csv_path;
   }
   return "";
}

static BestKnownObjective read_best_known_objective_file(const std::string& csv_filename, const std::string& instance_path) {
   BestKnownObjective e = find_best_known_for_instance(csv_filename, instance_path);
   if (e.loaded) {
      e.filename = csv_filename;
      if (e.matched_instance.empty()) e.matched_instance = e.instance;
   }
   return e;
}

Instance read_instance(const std::string& filename, ObjectiveSense sense) {
   std::ifstream in(filename);
   if (!in) {
      throw std::runtime_error("Cannot open file: " + filename);
   }
   std::vector<std::string> lines;
   std::string line;
   while (std::getline(in, line)) {
      line = trim(line);
      if (line.empty()) continue;
      if (starts_with_hash(line)) continue;
      lines.push_back(line);
   }
   if (lines.empty()) {
      throw std::runtime_error("QSC file has no non-comment lines.");
   }
   struct Entry {
      int i, j;
      double v;
   };
   Instance ins;
   int declared_num_entries = -1;
   size_t first_entry_line = 0;
   int detected_type = 0;
   std::vector<std::string> first_tokens;
   {
      std::istringstream iss(lines[0]);
      std::string tok;
      while (iss >> tok) {
         first_tokens.push_back(tok);
      }
   }
   if (first_tokens.size() == 1) {
      if (lines.size() < 2) {
         throw std::runtime_error("Type 2 format needs at least two header lines.");
      }
      {
         std::istringstream iss(lines[0]);
         if (!(iss >> ins.k)) {
            throw std::runtime_error("Failed to read k from first line.");
         }
      }
      {
         std::istringstream iss(lines[1]);
         if (!(iss >> ins.n >> declared_num_entries)) {
            throw std::runtime_error("Failed to read n and number_of_entries from second line for Type 2 format.");
         }
      }
      first_entry_line = 2;
      detected_type = 2;
   }
   else if (first_tokens.size() == 2) {
      std::istringstream iss(lines[0]);
      if (!(iss >> ins.n >> ins.k)) {
         throw std::runtime_error("Failed to read n and k from first line for Type 1 format.");
      }
      declared_num_entries = -1; 
      first_entry_line = 1;
      detected_type = 1;
   }
   else if (first_tokens.size() == 3) {
      std::istringstream iss(lines[0]);
      if (!(iss >> ins.n >> ins.k >> declared_num_entries)) {
         throw std::runtime_error("Failed to read n, k, and number_of_entries from first line for Type 3 format.");
      }
      first_entry_line = 1;
      detected_type = 3;
   }
   else {
      throw std::runtime_error( "Cannot detect data format." );
   }

   if (ins.n < 0) {
      throw std::runtime_error("Invalid n: must be nonnegative.");
   }

   if (ins.k < 0 || ins.k > ins.n) {
      throw std::runtime_error("Invalid k: must satisfy 0 <= k <= n.");
   }

   if (declared_num_entries < -1) {
      throw std::runtime_error("Invalid number_of_entries.");
   }

   std::vector<Entry> entries;
   if (declared_num_entries > 0) {
      entries.reserve(static_cast<size_t>(declared_num_entries));
   }

   int min_index = std::numeric_limits<int>::max();
   int max_index = std::numeric_limits<int>::min();

   for (size_t t = first_entry_line; t < lines.size(); ++t) {
      std::istringstream iss(lines[t]);

      int i, j;
      double v;

      if (!(iss >> i >> j >> v)) {
         throw std::runtime_error(
            "Invalid entry line while reading file: " + lines[t]
         );
      }

      entries.push_back({ i, j, v });

      min_index = std::min(min_index, std::min(i, j));
      max_index = std::max(max_index, std::max(i, j));
   }

   if (declared_num_entries >= 0 &&
      static_cast<int>(entries.size()) != declared_num_entries) {
      std::cerr << "WARNING: declared number_of_entries = " << declared_num_entries << ", but actually read " << entries.size() << " entries.\n";
   }

   int shift = 0;

   if (!entries.empty()) {
      const bool valid_zero_based =
         (min_index >= 0 && max_index <= ins.n - 1);

      const bool valid_one_based =
         (min_index >= 1 && max_index <= ins.n);

      if (valid_zero_based && !valid_one_based) {
         shift = 0;
      }
      else if (!valid_zero_based && valid_one_based) {
         shift = 1;
      }
      else if (valid_zero_based && valid_one_based) {
         if (min_index == 0) {
            shift = 0;
         }
         else {
            shift = 1;
            std::cerr << "WARNING: indexing is ambiguous";
         }
      }
      else {
         std::ostringstream oss;
         oss << "Cannot detect index base. Index range in file is [" << min_index << ", " << max_index << "], but n = " << ins.n << ".";
         throw std::runtime_error(oss.str());
      }
   }

   std::vector<Entry> full_entries;
   full_entries.reserve(entries.size() * 2);

   for (const auto& e : entries) {
      const int i = e.i - shift;
      const int j = e.j - shift;

      if (i < 0 || i >= ins.n || j < 0 || j >= ins.n) {
         std::ostringstream oss;
         oss << "Index out of range while reading file. "
            << "Original entry: (" << e.i << ", " << e.j << "), "
            << "after shift: (" << i << ", " << j << "), "
            << "n = " << ins.n << ".";
         throw std::runtime_error(oss.str());
      }

      const double q = (sense == ObjectiveSense::Max) ? -e.v : e.v;
      if (q == 0.0) continue;

      full_entries.push_back({ i, j, q });
      if (i != j) {
         full_entries.push_back({ j, i, q });
      }
   }

   std::sort(full_entries.begin(), full_entries.end(), [](const Entry& a, const Entry& b) {
      if (a.i != b.i) return a.i < b.i;
      return a.j < b.j;
   });

   std::vector<Entry> nz;
   nz.reserve(full_entries.size());
   for (const auto& e : full_entries) {
      if (!nz.empty() && nz.back().i == e.i && nz.back().j == e.j) {
         nz.back().v += e.v;
      }
      else {
         nz.push_back(e);
      }
   }

   nz.erase(std::remove_if(nz.begin(), nz.end(), [](const Entry& e) {
      return e.v == 0.0;
   }), nz.end());

   ins.nnz_full = nz.size();
   const double denom = static_cast<double>(ins.n) * static_cast<double>(ins.n);
   ins.density = (denom > 0.0) ? static_cast<double>(ins.nnz_full) / denom : 0.0;
   ins.sparse = (ins.density <= SPARSE_DENSITY_THRESHOLD);

   if (ins.sparse) {
      ins.Q.clear();
      ins.row_ptr.assign(static_cast<size_t>(ins.n) + 1, 0);
      ins.col_ind.resize(ins.nnz_full);
      ins.val.resize(ins.nnz_full);

      for (const auto& e : nz) {
         ins.row_ptr[static_cast<size_t>(e.i) + 1]++;
      }

      for (int i = 0; i < ins.n; ++i) {
         ins.row_ptr[i + 1] += ins.row_ptr[i];
      }

      std::vector<int> next = ins.row_ptr;
      for (const auto& e : nz) {
         const int pos = next[e.i]++;
         ins.col_ind[pos] = e.j;
         ins.val[pos] = e.v;
      }
   }
   else {
      ins.row_ptr.clear();
      ins.col_ind.clear();
      ins.val.clear();
      ins.Q.assign(static_cast<size_t>(ins.n) * static_cast<size_t>(ins.n), 0.0);
      for (const auto& e : nz) {
         Qref(ins, e.i, e.j) = e.v;
      }
   }

   std::cerr << "Data type = " << detected_type
      << ", n = " << ins.n
      << ", k = " << ins.k
      << ", input entries = " << entries.size()
      << ", full nnz = " << ins.nnz_full
      << ", density = " << std::fixed << std::setprecision(6) << (100.0 * ins.density) << "%"
      << ", storage = " << (ins.sparse ? "CSR sparse" : "dense matrix")
      << ", index base = " << (shift == 0 ? 0 : 1)
      << "-based\n";

   return ins;
}

double objective_x(const std::vector<int>& x, const Instance& ins) {
   const int n = static_cast<int>(x.size());
   double obj = 0.0;

   if (ins.sparse) {
      for (int i = 0; i < n; ++i) {
         if (!x[i]) continue;
         for (int p = ins.row_ptr[i]; p < ins.row_ptr[i + 1]; ++p) {
            if (x[ins.col_ind[p]]) obj += ins.val[p];
         }
      }
      return obj;
   }

   for (int i = 0; i < n; ++i) {
      if (!x[i]) continue;
      obj += Qat(ins, i, i);
      for (int j = i + 1; j < n; ++j) {
         if (x[j]) obj += 2.0 * Qat(ins, i, j);
      }
   }
   return obj;
}

std::vector<double> compute_h_x(const std::vector<int>& x, const Instance& ins) {
   const int n = static_cast<int>(x.size());
   std::vector<double> h(n, 0.0);

   if (ins.sparse) {
      for (int i = 0; i < n; ++i) {
         double s = 0.0;
         for (int p = ins.row_ptr[i]; p < ins.row_ptr[i + 1]; ++p) {
            s += ins.val[p] * static_cast<double>(x[ins.col_ind[p]]);
         }
         h[i] = s;
      }
      return h;
   }

   for (int i = 0; i < n; ++i) {
      const double* row = &ins.Q[static_cast<size_t>(i) * static_cast<size_t>(ins.n)];
      double s = 0.0;
      for (int j = 0; j < n; ++j) {
         s += row[j] * static_cast<double>(x[j]);
      }
      h[i] = s;
   }
   return h;
}

static inline void add_symmetric_column_to_h(std::vector<double>& h, const Instance& ins, int col, double scale) {
   if (scale == 0.0) return;

   if (ins.sparse) {
      for (int p = ins.row_ptr[col]; p < ins.row_ptr[col + 1]; ++p) {
         h[ins.col_ind[p]] += scale * ins.val[p];
      }
      return;
   }

   const double* row = &ins.Q[static_cast<size_t>(col) * static_cast<size_t>(ins.n)];
   for (int l = 0; l < ins.n; ++l) {
      h[l] += scale * row[l];
   }
}

std::vector<double> interaction_impact(const Instance& ins) {
   const int n = ins.n;
   std::vector<double> imp(n, 0.0);

   const double scale = (n > 1) ? (2.0 * static_cast<double>(ins.k - 1) / static_cast<double>(n - 1)) : 0.0;

   #pragma omp parallel for schedule(static)
   for (int i = 0; i < n; ++i) {
      double diag = 0.0;
      double off_sum = 0.0;

      if (ins.sparse) {
         for (int p = ins.row_ptr[i]; p < ins.row_ptr[i + 1]; ++p) {
            if (ins.col_ind[p] == i) diag += ins.val[p];
            else off_sum += ins.val[p];
         }
      }
      else {
         const double* row = &ins.Q[static_cast<size_t>(i) * static_cast<size_t>(n)];
         diag = row[i];
         for (int j = 0; j < n; ++j) {
            if (i != j) off_sum += row[j];
         }
      }

      imp[i] = diag + scale * off_sum;
   }
   return imp;
}

std::vector<int> build_set(const Instance& ins, const Params& par, const std::vector<double>& impact) {
   std::vector<int> idx(ins.n);
   std::iota(idx.begin(), idx.end(), 0);

   std::sort(idx.begin(), idx.end(), [&](int a, int b) {
      return impact[a] < impact[b];
   });

   int v_0 = static_cast<int>(std::ceil(par.v_0_frac * ins.n));
   v_0 = std::min(v_0, ins.k);

   std::vector<int> C;
   C.reserve(v_0);
   for (int t = 0; t < v_0; ++t) C.push_back(idx[t]);
   return C;
}

int hamming_distance(const std::vector<int>& a, const std::vector<int>& b) {
   int n = static_cast<int>(a.size());
   int d = 0;
   for (int i = 0; i < n; ++i) {
      if (a[i] != b[i]) ++d;
   }
   return d;
}

void add_to_elite_count(const std::vector<int>& x, std::vector<int>& elite_ones_count) {
   const int n = static_cast<int>(x.size());
   for (int i = 0; i < n; ++i) {
      if (x[i] == 1) {
         elite_ones_count[i]++;
      }
   }
}

void remove_from_elite_count(const std::vector<int>& x, std::vector<int>& elite_ones_count) {
   const int n = static_cast<int>(x.size());
   for (int i = 0; i < n; ++i) {
      if (x[i] == 1) {
         elite_ones_count[i]--;
      }
   }
}

void insert_elite_pool(const Instance& ins, std::vector<EliteSol>& elite_pool, std::vector<int>& elite_ones_count, const std::vector<int>& x, double obj, const Params& par) {
   const double EPS = 1e-6;
   for (const auto& e : elite_pool) {
      if (e.x == x) return;
   }
   auto sort_elite_pool = [&]() {
      std::sort(elite_pool.begin(), elite_pool.end(),
         [](const EliteSol& a, const EliteSol& b) {
            return a.obj < b.obj;
         });
      };

   auto min_hamming_excluding = [&](int excluded_idx) {
      int dmin = std::numeric_limits<int>::max();
      for (int r = 0; r < static_cast<int>(elite_pool.size()); ++r) {
         if (r == excluded_idx) continue;
         dmin = std::min(dmin, hamming_distance(x, elite_pool[r].x));
      }
      return dmin;
      };

   auto member_min_hamming_excluding = [&](int member_idx) {
      int dmin = std::numeric_limits<int>::max();
      for (int r = 0; r < static_cast<int>(elite_pool.size()); ++r) {
         if (r == member_idx) continue;
         dmin = std::min(dmin, hamming_distance(elite_pool[member_idx].x, elite_pool[r].x));
      }
      return dmin;
      };

   const int d_0 = static_cast<int>(std::ceil(par.d_frac * static_cast<double>(ins.k)));

   const int relaxed_threshold = std::max(2, static_cast<int>(std::ceil(0.80 * static_cast<double>(d_0))));

   if (elite_pool.empty()) {
      elite_pool.push_back({ x, obj });
      add_to_elite_count(x, elite_ones_count);
      return;
   }

   sort_elite_pool();
   const double best_obj = elite_pool.front().obj;
   const bool new_global_best = (obj < best_obj - EPS);

   int min_dist = std::numeric_limits<int>::max();
   for (const auto& e : elite_pool) {
      min_dist = std::min(min_dist, hamming_distance(x, e.x));
   }

   if (static_cast<int>(elite_pool.size()) < 30) {
      const bool enough_diversity = (min_dist >= d_0);
      const bool good_and_comparable_diversity =
         new_global_best && (min_dist >= relaxed_threshold);

      if (enough_diversity || good_and_comparable_diversity) {
         elite_pool.push_back({ x, obj });
         add_to_elite_count(x, elite_ones_count);
         sort_elite_pool();
      }
      return;
   }

   int replace_idx = -1;
   double best_replacement_key = std::numeric_limits<double>::infinity();

   for (int r = 0; r < static_cast<int>(elite_pool.size()); ++r) {
      const int cand_dist_to_others = min_hamming_excluding(r);
      const int old_dist_to_others = member_min_hamming_excluding(r);

      const int old_comparable_dist =
         (old_dist_to_others == std::numeric_limits<int>::max())
         ? d_0
         : static_cast<int>(std::floor(0.80 * static_cast<double>(old_dist_to_others)));

      const bool full_diversity_after_replacement = (cand_dist_to_others >= d_0);

      const bool comparable_diversity_after_replacement = (cand_dist_to_others >= relaxed_threshold) &&  (cand_dist_to_others >= old_comparable_dist);

      const bool better_than_this_member = (obj < elite_pool[r].obj - EPS);
      const bool acceptable_quality = (obj <= elite_pool.back().obj + EPS);

      const bool can_replace =
         (acceptable_quality && full_diversity_after_replacement) ||
         (better_than_this_member && comparable_diversity_after_replacement) ||
         (new_global_best && cand_dist_to_others >= relaxed_threshold);

      if (!can_replace) continue;

      const double quality_gain = elite_pool[r].obj - obj; 
      const double replacement_key = -quality_gain - 1e-6 * static_cast<double>(cand_dist_to_others);

      if (replacement_key < best_replacement_key) {
         best_replacement_key = replacement_key;
         replace_idx = r;
      }
   }

   if (replace_idx >= 0) {
      remove_from_elite_count(elite_pool[replace_idx].x, elite_ones_count);
      elite_pool[replace_idx] = { x, obj };
      add_to_elite_count(x, elite_ones_count);
      sort_elite_pool();
   }
}

static int tabu_tenure_T(int T_nimp, const Params& par) {
   int T;
   if (T_nimp == 0) {
      T = par.t_alpha;
   }
   else if (T_nimp < 100) {
      T = 2 * par.t_alpha;
   }
   else {
      T = 3 * par.t_alpha;
   }
   return T;
}

void tabu_search(
   std::vector<int>& x,
   const Instance& ins,
   double& obj,
   const Params& par
) {
   const int n = static_cast<int>(x.size());
   const int k = std::accumulate(x.begin(), x.end(), 0);

   if (k != ins.k) {
      throw std::runtime_error("tabu_search: x has wrong cardinality.");
   }

   std::vector<double> h = compute_h_x(x, ins);
   std::vector<int> tabu_until(n, -1);
   std::vector<int> ones;
   std::vector<int> zeros;
   std::vector<int> pos_in_ones(n, -1);
   std::vector<int> pos_in_zeros(n, -1);
   ones.reserve(k);
   zeros.reserve(n - k);

   for (int i = 0; i < n; ++i) {
      if (x[i]) {
         pos_in_ones[i] = static_cast<int>(ones.size());
         ones.push_back(i);
      }
      else {
         pos_in_zeros[i] = static_cast<int>(zeros.size());
         zeros.push_back(i);
      }
   }

   auto remove_from_vec = [](std::vector<int>& vec, std::vector<int>& pos, int v) {
      const int p = pos[v];
      const int last = vec.back();
      vec[p] = last;
      pos[last] = p;
      vec.pop_back();
      pos[v] = -1;
      };

   auto add_to_vec = [](std::vector<int>& vec, std::vector<int>& pos, int v) {
      pos[v] = static_cast<int>(vec.size());
      vec.push_back(v);
      };

   std::vector<int> best_x = x;
   double best_obj = obj;
   std::vector<int> cand_out;
   std::vector<int> cand_in;
   cand_out.reserve(n);
   cand_in.reserve(n);
   int no_improve_tabu = 0;
   for (int iter = 0; iter < par.T_max; ++iter) {
      if (ones.empty() || zeros.empty()) break;
      cand_out = ones;
      cand_in = zeros;
      const int num_cand_out = std::min(par.v_1, static_cast<int>(cand_out.size()));
      const int num_cand_in = std::min(par.v_2, static_cast<int>(cand_in.size()));

      if (num_cand_out < static_cast<int>(cand_out.size())) {
         std::nth_element(
            cand_out.begin(),
            cand_out.begin() + num_cand_out,
            cand_out.end(),
            [&](int a, int b) {
               return h[a] > h[b]; 
            }
         );
      }
      cand_out.resize(num_cand_out);

      if (num_cand_in < static_cast<int>(cand_in.size())) {
         std::nth_element(
            cand_in.begin(),
            cand_in.begin() + num_cand_in,
            cand_in.end(),
            [&](int a, int b) {
               return h[a] < h[b]; 
            }
         );
      }
      cand_in.resize(num_cand_in);
      int best_i = -1;
      int best_j = -1;
      double best_delta = std::numeric_limits<double>::infinity();

      for (int i : cand_out) {
         for (int j : cand_in) {
            const double delta = 2.0 * (h[j] - h[i]) + Qat(ins, i, i) + Qat(ins, j, j) - 2.0 * Qat(ins, i, j);
            const double new_obj = obj + delta;
            const bool is_tabu = (iter <= tabu_until[i] || iter <= tabu_until[j]);
            const bool aspiration = (new_obj < best_obj - 1e-6);
            if (is_tabu && !aspiration) continue;
            if (delta < best_delta) {
               best_delta = delta;
               best_i = i;
               best_j = j;
            }
         }
      }

      if (best_i < 0 || best_j < 0) {
         for (int i : cand_out) {
            for (int j : cand_in) {
               const double delta = 2.0 * (h[j] - h[i]) + Qat(ins, i, i) + Qat(ins, j, j) - 2.0 * Qat(ins, i, j);
               if (delta < best_delta) {
                  best_delta = delta;
                  best_i = i;
                  best_j = j;
               }
            }
         }
      }

      if (best_i < 0 || best_j < 0) break;

      x[best_i] = 0;
      x[best_j] = 1;
      obj += best_delta;

      add_symmetric_column_to_h(h, ins, best_i, -1.0);
      add_symmetric_column_to_h(h, ins, best_j, 1.0);

      remove_from_vec(ones, pos_in_ones, best_i);
      add_to_vec(zeros, pos_in_zeros, best_i);
      remove_from_vec(zeros, pos_in_zeros, best_j);
      add_to_vec(ones, pos_in_ones, best_j);

      if (obj < best_obj - 1e-6) {
         best_obj = obj;
         best_x = x;
         no_improve_tabu = 0;
      }
      else {
         ++no_improve_tabu;
      }

      if (no_improve_tabu >= 2000) {
         break;
      }
      
      const int T = tabu_tenure_T(no_improve_tabu, par);

      tabu_until[best_i] = iter + T;
      tabu_until[best_j] = iter + int (T * 2/3);
   }

   x = best_x;
   obj = best_obj;
   
}

std::vector<int> path_relink_one_direction(
   const Instance& ins,
   const std::vector<int>& start,
   const std::vector<int>& target,
   const Params& par,
   std::mt19937_64& rng
) {
   const int n = ins.n;

   if (static_cast<int>(start.size()) != n ||
      static_cast<int>(target.size()) != n) {
      throw std::runtime_error("path_relink_one_direction: invalid solution size.");
   }

   if (std::accumulate(start.begin(), start.end(), 0) != ins.k ||
      std::accumulate(target.begin(), target.end(), 0) != ins.k) {
      throw std::runtime_error("path_relink_one_direction: wrong cardinality.");
   }

   if (start == target) {
      return start;
   }

   std::vector<int> x = start;
   double obj = objective_x(x, ins);
   std::vector<double> h = compute_h_x(x, ins);
   bool found_intermediate = false;
   std::vector<int> best_x = start;
   double best_obj = std::numeric_limits<double>::infinity();
   std::vector<int> remove_set;
   std::vector<int> add_set;
   remove_set.reserve(ins.k);
   add_set.reserve(ins.k);

   for (int i = 0; i < n; ++i) {
      if (start[i] == 1 && target[i] == 0) {
         remove_set.push_back(i);
      }
      else if (start[i] == 0 && target[i] == 1) {
         add_set.push_back(i);
      }
   }

   auto erase_unordered = [](std::vector<int>& v, int pos) {
      v[pos] = v.back();
      v.pop_back();
      };

   struct Move {
      int i;
      int j;
      double delta;
   };

   while (!remove_set.empty() && !add_set.empty()) {
      const int A = std::min(par.v_1, static_cast<int>(remove_set.size()));
      const int B = std::min(par.v_2, static_cast<int>(add_set.size()));

      std::vector<int> cand_remove = remove_set;
      if (A < static_cast<int>(cand_remove.size())) {
         std::nth_element(
            cand_remove.begin(),
            cand_remove.begin() + A,
            cand_remove.end(),
            [&](int a, int b) {
               return h[a] > h[b];
            }
         );
      }
      cand_remove.resize(A);

      std::vector<int> cand_add = add_set;
      if (B < static_cast<int>(cand_add.size())) {
         std::nth_element(
            cand_add.begin(),
            cand_add.begin() + B,
            cand_add.end(),
            [&](int a, int b) {
               return h[a] < h[b];
            }
         );
      }
      cand_add.resize(B);

      std::vector<Move> moves;
      moves.reserve(static_cast<size_t>(cand_remove.size()) * static_cast<size_t>(cand_add.size()));

      for (int i : cand_remove) {
         for (int j : cand_add) {
            const double delta =
               2.0 * (h[j] - h[i])
               + Qat(ins, i, i)
               + Qat(ins, j, j)
               - 2.0 * Qat(ins, i, j);

            moves.push_back({ i, j, delta });
         }
      }

      if (moves.empty()) {
         break;
      }

      std::sort(moves.begin(), moves.end(),
         [](const Move& a, const Move& b) {
            return a.delta < b.delta;
         }
      );

      const int R = std::min<int>(10, static_cast<int>(moves.size()));
      std::uniform_int_distribution<int> pick(0, R - 1);
      const Move mv = moves[pick(rng)];

      x[mv.i] = 0;
      x[mv.j] = 1;
      obj += mv.delta;

      add_symmetric_column_to_h(h, ins, mv.i, -1.0);
      add_symmetric_column_to_h(h, ins, mv.j, 1.0);

      for (int p = 0; p < static_cast<int>(remove_set.size()); ++p) {
         if (remove_set[p] == mv.i) {
            erase_unordered(remove_set, p);
            break;
         }
      }

      for (int p = 0; p < static_cast<int>(add_set.size()); ++p) {
         if (add_set[p] == mv.j) {
            erase_unordered(add_set, p);
            break;
         }
      }

      const bool is_target = (remove_set.empty() && add_set.empty());

      if (!is_target) {
         if (!found_intermediate || obj < best_obj) {
            found_intermediate = true;
            best_obj = obj;
            best_x = x;
         }
      }
   }

   if (!found_intermediate) {
      return start;
   }

   return best_x;
}

std::vector<int> path_relinking(const Instance& ins, const std::vector<int>& x_current, const std::vector<EliteSol>& elite_pool, const Params& par, std::mt19937_64& rng) {
   const int n = ins.n;

   if (static_cast<int>(x_current.size()) != n) {
      throw std::runtime_error("path_relinking: x_current has invalid size.");
   }

   if (std::accumulate(x_current.begin(), x_current.end(), 0) != ins.k) {
      throw std::runtime_error("path_relinking: x_current has wrong cardinality.");
   }

   if (elite_pool.empty()) {
      return x_current;
   }

   const int d_0 = static_cast<int>( std::ceil(par.d_frac * ins.k));

   double best_elite_obj = elite_pool.front().obj;
   double worst_elite_obj = elite_pool.back().obj;


   const double obj_denom =
      std::max(1e-6, worst_elite_obj - best_elite_obj);

   int guide_idx = -1;
   int best_dist = -1;
   double best_score = -std::numeric_limits<double>::infinity();

   for (int e = 0; e < static_cast<int>(elite_pool.size()); ++e) {
      const int d = hamming_distance(x_current, elite_pool[e].x);

      if (d == 0) {
         continue;
      }

      if (d < d_0) {
         continue;
      }

      const double quality_score = (worst_elite_obj - elite_pool[e].obj) / obj_denom;

      const double distance_score = static_cast<double>(d) / static_cast<double>(2 * ins.k);

      const double score = quality_score + distance_score;

      if (score > best_score) {
         best_score = score;
         best_dist = d;
         guide_idx = e;
      }
   }

   if (guide_idx < 0) {
      for (int e = 0; e < static_cast<int>(elite_pool.size()); ++e) {
         const int d = hamming_distance(x_current, elite_pool[e].x);

         if (d > best_dist) {
            best_dist = d;
            guide_idx = e;
         }
      }
   }

   if (guide_idx < 0 || best_dist <= 0) {
      return x_current;
   }

   const std::vector<int>& guide = elite_pool[guide_idx].x;

   std::vector<int> best_forward = path_relink_one_direction(ins, x_current, guide, par, rng);

   std::vector<int> best_backward = path_relink_one_direction(ins, guide, x_current, par, rng);

   const bool forward_changed = (best_forward != x_current);
   const bool backward_changed = (best_backward != guide);

   if (!forward_changed && !backward_changed) {
      return x_current;
   }

   if (forward_changed && !backward_changed) {
      return best_forward;
   }

   if (!forward_changed && backward_changed) {
      return best_backward;
   }

   const double obj_forward = objective_x(best_forward, ins);
   const double obj_backward = objective_x(best_backward, ins);

   if (obj_forward <= obj_backward) {
      return best_forward;
   }

   return best_backward;
}

std::vector<int> random_greedy_construction(const Instance& ins, const std::vector<int>& C2, double random_frac, const Params& par, const std::vector<EliteSol>& elite_pool, std::mt19937_64& rng) {
   const int n = ins.n;
   const int k = ins.k;

   int num_random = static_cast<int>(std::round(random_frac * k));
   num_random = std::max(0, std::min(num_random, k));

   std::vector<int> cand = C2;

   std::sort(cand.begin(), cand.end());
   cand.erase(std::unique(cand.begin(), cand.end()), cand.end());

   num_random = std::min(num_random, static_cast<int>(cand.size()));

   const int d_0 = static_cast<int>(std::ceil(par.d_frac * ins.k));

   const int max_attempts = elite_pool.empty() ? 1 : 200;

   std::vector<int> best_x(n, 0);
   int best_min_dist = -1;

   for (int attempt = 0; attempt < max_attempts; ++attempt) {
      std::vector<int> x(n, 0);
      std::vector<int> cand_attempt = cand;
      std::shuffle(cand_attempt.begin(), cand_attempt.end(), rng);
      int ones_now = 0;

      for (int t = 0; t < num_random; ++t) {
         int i = cand_attempt[t];
         if (x[i] == 0) {
            x[i] = 1;
            ++ones_now;
         }
      }

      std::vector<double> h = compute_h_x(x, ins);

      while (ones_now < k) {
         struct AddMove {
            int i;
            double gain;
         };

         std::vector<AddMove> moves;
         moves.reserve(n);

         for (int i = 0; i < n; ++i) {
            if (x[i]) continue;

            double gain = Qat(ins, i, i) + 2.0 * h[i];
            moves.push_back({ i, gain });
         }

         if (moves.empty()) {
            throw std::runtime_error("random_greedy_construction: no free variable left.");
         }

         std::sort(moves.begin(), moves.end(),
            [](const AddMove& a, const AddMove& b) {
               return a.gain < b.gain;
            }
         );

         int R = std::min(20, static_cast<int>(moves.size()));
         std::uniform_int_distribution<int> pick(0, R - 1);

         int best_i = moves[pick(rng)].i;

         x[best_i] = 1;
         ++ones_now;

          add_symmetric_column_to_h(h, ins, best_i, 1.0);
      }

      if (elite_pool.empty()) {
         return x;
      }

      int cur_min_dist = std::numeric_limits<int>::max();
      for (const auto& e : elite_pool) {
         cur_min_dist = std::min(cur_min_dist, hamming_distance(x, e.x));
      }

      if (cur_min_dist >= d_0) {
         return x;
      }

      if (cur_min_dist > best_min_dist) {
         best_min_dist = cur_min_dist;
         best_x = std::move(x);
      }
   }

   return best_x;
}

std::vector<int> elite_based_restart(const Instance& ins, const Params& par, const std::vector<EliteSol>& elite_pool, std::mt19937_64& rng) {
   const int n = ins.n;
   const int k = ins.k;

   if (elite_pool.empty()) {
      std::vector<int> idx(n);
      std::iota(idx.begin(), idx.end(), 0);
      std::shuffle(idx.begin(), idx.end(), rng);

      std::vector<int> x(n, 0);
      for (int t = 0; t < k; ++t) {
         x[idx[t]] = 1;
      }
      return x;
   }

   std::vector<int> elite_freq(n, 0);
   for (const auto& e : elite_pool) {
      if (static_cast<int>(e.x.size()) != n) {
         throw std::runtime_error("elite_based_restart: invalid elite solution size.");
      }

      for (int i = 0; i < n; ++i) {
         if (e.x[i]) {
            ++elite_freq[i];
         }
      }
   }

   const int elite_size = static_cast<int>(elite_pool.size());
   const int freq_threshold = static_cast<int>(std::ceil(0.85 * static_cast<double>(elite_size)));

   std::vector<int> fixed_ones;
   fixed_ones.reserve(k);

   for (int i = 0; i < n; ++i) {
      if (elite_freq[i] >= freq_threshold) {
         fixed_ones.push_back(i);
      }
   }

   std::sort(fixed_ones.begin(), fixed_ones.end(),
      [&](int a, int b) {
         if (elite_freq[a] != elite_freq[b]) {
            return elite_freq[a] > elite_freq[b];
         }
         return a < b;
      }
   );

   if (static_cast<int>(fixed_ones.size()) > k) {
      fixed_ones.resize(k);
   }

   std::vector<char> is_fixed(n, 0);
   std::vector<int> free_vars;
   free_vars.reserve(n);

   for (int i : fixed_ones) {
      is_fixed[i] = 1;
   }

   for (int i = 0; i < n; ++i) {
      if (!is_fixed[i]) {
         free_vars.push_back(i);
      }
   }

   const int need = k - static_cast<int>(fixed_ones.size());
   if (need < 0 || need > static_cast<int>(free_vars.size())) {
      throw std::runtime_error("elite_based_restart: infeasible elite fixing.");
   }

   const int d_1 = static_cast<int>(std::ceil(par.d_frac * k));

   const int max_attempts = 200;

   std::vector<int> best_x(n, 0);
   int best_min_dist = -1;

   for (int attempt = 0; attempt < max_attempts; ++attempt) {
      std::vector<int> x(n, 0);

      for (int i : fixed_ones) {
         x[i] = 1;
      }

      std::vector<int> cand = free_vars;
      std::shuffle(cand.begin(), cand.end(), rng);

      for (int t = 0; t < need; ++t) {
         x[cand[t]] = 1;
      }

      int cur_min_dist = std::numeric_limits<int>::max();
      for (const auto& e : elite_pool) {
         cur_min_dist = std::min(cur_min_dist, hamming_distance(x, e.x));
      }

      if (cur_min_dist >= d_1) {
         return x;
      }

      if (cur_min_dist > best_min_dist) {
         best_min_dist = cur_min_dist;
         best_x = std::move(x);
      }
   }

   return best_x;
}

std::vector<int> random_diverse_restart(const Instance& ins, const Params& par, std::mt19937_64& rng, const std::vector<EliteSol>& elite_pool) {
   const int n = ins.n;
   const int k = ins.k;

   std::vector<int> idx(n);
   std::iota(idx.begin(), idx.end(), 0);

   const int d_1 = static_cast<int>(std::ceil(par.d_frac * ins.k));

   const int max_attempts = 200;

   std::vector<int> best_x(n, 0);
   int best_min_dist = -1;

   for (int attempt = 0; attempt < max_attempts; ++attempt) {
      std::shuffle(idx.begin(), idx.end(), rng);

      std::vector<int> x(n, 0);
      for (int t = 0; t < k; ++t) {
         x[idx[t]] = 1;
      }

      if (elite_pool.empty()) {
         return x;
      }

      int cur_min_dist = std::numeric_limits<int>::max();
      for (const auto& e : elite_pool) {
         cur_min_dist = std::min(cur_min_dist, hamming_distance(x, e.x));
      }

      if (cur_min_dist >= d_1) {
         return x;
      }

      if (cur_min_dist > best_min_dist) {
         best_min_dist = cur_min_dist;
         best_x = std::move(x);
      }
   }

   return best_x;
}

Result main_block(
   const Instance& ins,
   const Params& par,
   uint64_t seed,
   std::vector<EliteSol>& global_elite_pool,
   std::vector<int>& global_elite_ones_count,
   const std::vector<int>& C1,
   const std::chrono::steady_clock::time_point& t_start,
   ObjectiveSense sense,
   PrintScale scale,
   const BestKnownObjective& best_known,
   std::atomic<bool>& stop_requested
) {
   std::mt19937_64 rng(seed);

   Result res;
   int no_improve = 0;
   std::vector<int> incumbent_for_guide;
   std::vector<EliteSol> elite_snapshot;
   for (int start = 0; start < par.num_starts; ++start) {
      if (stop_requested.load(std::memory_order_relaxed)) break;
      if (time_limit_reached(t_start, par.time_limit_seconds)) break;
      std::vector<int> x;

      std::vector<int> elite_count_snapshot;

      #pragma omp critical
      {
         elite_snapshot = global_elite_pool;
         elite_count_snapshot = global_elite_ones_count;
      }

      if (!elite_snapshot.empty() && no_improve >= 10) {
         x = path_relinking(ins, incumbent_for_guide, elite_snapshot, par, rng);
         no_improve = 0;
      }
      else {
         if (!incumbent_for_guide.empty() && (start % 10 == 0)) {
            x = elite_based_restart(ins, par, elite_snapshot, rng);
         }
         else if (start % 10 == 9){
            x = random_diverse_restart(ins, par, rng, elite_snapshot);
         }
         else {
            std::vector<int> C2_local = C1;
            for (int i = 0; i < ins.n; ++i) {
               if (elite_count_snapshot[i] > 0) {
                  C2_local.push_back(i);
               }
            }
            std::sort(C2_local.begin(), C2_local.end());
            C2_local.erase(std::unique(C2_local.begin(), C2_local.end()), C2_local.end());
            x = random_greedy_construction(ins, C2_local, par.balance, par, elite_snapshot, rng);
         }
      }

      double obj = objective_x(x, ins);
      tabu_search(x, ins, obj, par);

      #pragma omp critical
      {
         insert_elite_pool(ins, global_elite_pool, global_elite_ones_count, x, obj, par);
      }

      if (obj < res.best_obj) {
         res.best_obj = obj;
         res.best_x = x;
         auto t_now = std::chrono::steady_clock::now();
         res.time_to_best = std::chrono::duration<double>(t_now - t_start).count();
         incumbent_for_guide = x;
         no_improve = 0;
         const double output_obj = output_objective_from_internal(res.best_obj, sense, scale);
         if (best_known.loaded && target_reached(output_obj, best_known.objective, sense)) {
            stop_requested.store(true, std::memory_order_relaxed);
         }
      }
      else {
         ++no_improve;
      }
   }
   return res;
}


static std::string csv_escape(const std::string& s) {
   bool need_quotes = false;
   for (char c : s) {
      if (c == ',' || c == '"' || c == '\n' || c == '\r') {
         need_quotes = true;
         break;
      }
   }
   if (!need_quotes) return s;

   std::string out = "\"";
   for (char c : s) {
      if (c == '"') out += "\"\"";
      else out.push_back(c);
   }
   out += "\"";
   return out;
}

static bool file_exists_for_append_header(const std::string& path) {
   std::error_code ec;
   return fs::exists(path, ec) && fs::is_regular_file(path, ec) && fs::file_size(path, ec) > 0;
}

static std::string default_report_csv_path() {
   std::error_code ec;

   fs::path stdout_target = fs::read_symlink("/proc/self/fd/1", ec);
   if (!ec && !stdout_target.empty()) {
      std::error_code ec_file;
      if (fs::is_regular_file(stdout_target, ec_file)) {
         fs::path log_dir = stdout_target.parent_path();
         if (!log_dir.empty()) {
            return (log_dir / "FCBQP_10runs_summary.csv").string();
         }
      }
   }

   return "FCBQP_10runs_summary.csv";
}

static void append_summary_csv_row(
   const std::string& report_csv,
   const std::string& instance_name,
   const std::string& best_known_str,
   double best_found_obj,
   double average_obj,
   const std::string& average_tts_str,
   int reached_count
) {
   const bool has_header = file_exists_for_append_header(report_csv);

   std::ofstream out(report_csv, std::ios::app);
   if (!out) {
      throw std::runtime_error("Cannot open report CSV for writing: " + report_csv);
   }

   if (!has_header) {
      out << "Instance,best-known obj,best found obj,average obj,average TTS,Times reaching best-known obj\n";
   }

   out << csv_escape(instance_name) << ","
       << csv_escape(best_known_str) << ","
       << std::fixed << std::setprecision(9) << best_found_obj << ","
       << std::fixed << std::setprecision(9) << average_obj << ","
       << csv_escape(average_tts_str) << ","
       << reached_count << "\n";
}

struct RunConfig {
   ObjectiveSense sense = ObjectiveSense::Max;
   PrintScale scale = PrintScale::Full;
   std::string instance_path;
   std::string best_known_arg = "best_known_results.csv";
   uint64_t base_seed = 1ULL;
   int num_runs = 10;
   std::string report_csv;
};

static Params params_for_instance(const Instance& ins) {
   Params par;
   par.num_starts = 200000;
   par.v_0_frac = 0.40;

   if (ins.n <= 7000) {
      par.T_max = 10000;
   }
   else {
      par.T_max = 20000;
   }

   if (ins.n <= 500) {
      par.t_alpha = 3;
   }
   else if (ins.n <= 2000) {
      par.t_alpha = 5;
   }
   else if (ins.n <= 5000) {
      par.t_alpha = 7;
   }
   else if (ins.n <= 8000) {
      par.t_alpha = 9;
   }
   else if (ins.n <= 20000) {
      par.t_alpha = 15;
   }
   else {
      par.t_alpha = std::max(1, static_cast<int>(0.2 * sqrt(std::min(ins.k, ins.n - ins.k))));
   }

   if (ins.n <= 7000) {
      par.v_1 = std::min(std::max(80, int(ins.k / 3)), 100);
      par.v_2 = std::min(std::max(120, int((ins.n - ins.k) / 12)), 200);
   }
   else {
      par.v_1 = std::min(std::max(80, int(ins.k / 3)), 300);
      par.v_2 = std::min(std::max(120, int((ins.n - ins.k) / 12)), 300);
   }

   par.time_limit_seconds = 600.0;
   return par;
}

static int run_single_instance(const RunConfig& cfg) {
   if (cfg.num_runs <= 0) {
      throw std::runtime_error("Number of runs must be positive.");
   }

   Instance ins = read_instance(cfg.instance_path, cfg.sense);
   const std::string best_known_path = resolve_best_known_objective_path(cfg.instance_path, cfg.best_known_arg);
   BestKnownObjective best_known;
   if (!best_known_path.empty()) {
      best_known = read_best_known_objective_file(best_known_path, cfg.instance_path);
   }

   Params par = params_for_instance(ins);
   const std::vector<double> impact = interaction_impact(ins);
   const std::vector<int> C1 = build_set(ins, par, impact);

   std::vector<double> run_output_objs;
   run_output_objs.reserve(cfg.num_runs);
   std::vector<double> reached_tts;
   reached_tts.reserve(cfg.num_runs);

   double best_output_obj = (cfg.sense == ObjectiveSense::Max)
      ? -std::numeric_limits<double>::infinity()
      :  std::numeric_limits<double>::infinity();
   Result best_run_res;
   best_run_res.best_obj = std::numeric_limits<double>::infinity();

   std::cout << "Instance: " << cfg.instance_path << "\n";
   std::cout << "Sense: " << objective_sense_name(cfg.sense) << "\n";
   std::cout << "Scale: " << print_scale_name(cfg.scale) << "\n";
   std::cout << "Runs: " << cfg.num_runs << "\n";
   std::cout << "Summary file: " << cfg.report_csv << "\n";

   if (best_known.loaded) {
      std::cout << "Best-known objective = " << std::fixed << std::setprecision(9) << best_known.objective << "\n";
   }
   else {
      std::cout << "Best-known objective file = not found or no matching instance\n";
   }

   for (int run = 1; run <= cfg.num_runs; ++run) {
      auto t0 = std::chrono::steady_clock::now();

      Result global_res;
      global_res.best_obj = std::numeric_limits<double>::infinity();

      std::vector<EliteSol> global_elite_pool;
      std::vector<int> global_elite_ones_count(ins.n, 0);
      std::atomic<bool> stop_requested(false);

      #pragma omp parallel
      {
         const int tid = omp_get_thread_num();
         const int num_threads = omp_get_num_threads();

         Params local_par = par;
         const int base = par.num_starts / num_threads;
         const int extra = par.num_starts % num_threads;

         local_par.num_starts = base + (tid < extra ? 1 : 0);

         const uint64_t run_seed = cfg.base_seed
            + 0xD1B54A32D192ED03ULL * static_cast<uint64_t>(run)
            + 0x9E3779B97F4A7C15ULL * static_cast<uint64_t>(tid + 1);

         Result local_res = main_block(
            ins,
            local_par,
            run_seed,
            global_elite_pool,
            global_elite_ones_count,
            C1,
            t0,
            cfg.sense,
            cfg.scale,
            best_known,
            stop_requested
         );

         #pragma omp critical
         {
            if (local_res.best_obj < global_res.best_obj) {
               global_res = std::move(local_res);
            }
         }
      }

      auto t1 = std::chrono::steady_clock::now();
      const double elapsed = std::chrono::duration<double>(t1 - t0).count();
      const double output_obj = output_objective_from_internal(global_res.best_obj, cfg.sense, cfg.scale);
      const bool reached_best_known = best_known.loaded && target_reached(output_obj, best_known.objective, cfg.sense);

      run_output_objs.push_back(output_obj);
      if (reached_best_known) {
         reached_tts.push_back(global_res.time_to_best);
      }

      const bool improves_best = (cfg.sense == ObjectiveSense::Max)
         ? (output_obj > best_output_obj + 1e-6)
         : (output_obj < best_output_obj - 1e-6);
      if (improves_best) {
         best_output_obj = output_obj;
         best_run_res = std::move(global_res);
      }

      std::cout << "Run " << run << "/" << cfg.num_runs
                << ": best objective = " << std::fixed << std::setprecision(9) << output_obj
                << ", reached best-known = " << (reached_best_known ? "YES" : "NO")
                << ", TTS (s) = " << std::fixed << std::setprecision(6) << global_res.time_to_best
                << ", runtime (s) = " << std::fixed << std::setprecision(6) << elapsed
                << "\n";
   }

   double sum_obj = 0.0;
   for (double v : run_output_objs) sum_obj += v;
   const double average_obj = sum_obj / static_cast<double>(run_output_objs.size());

   const int reached_count = static_cast<int>(reached_tts.size());
   std::string average_tts_str = "unknown";
   double average_tts = 0.0;
   if (reached_count > 0) {
      double sum_tts = 0.0;
      for (double v : reached_tts) sum_tts += v;
      average_tts = sum_tts / static_cast<double>(reached_count);
      std::ostringstream oss;
      oss << std::fixed << std::setprecision(6) << average_tts;
      average_tts_str = oss.str();
   }

   std::string best_known_str = "";
   if (best_known.loaded) {
      std::ostringstream oss;
      oss << std::fixed << std::setprecision(9) << best_known.objective;
      best_known_str = oss.str();
   }

   const std::string instance_name = basename_without_extension(cfg.instance_path);
   append_summary_csv_row(
      cfg.report_csv,
      instance_name,
      best_known_str,
      best_output_obj,
      average_obj,
      average_tts_str,
      reached_count
   );

   std::cout << "\nSummary over " << cfg.num_runs << " runs\n";
   std::cout << "Instance name: " << instance_name << "\n";
   std::cout << "Best-known obj from CSV = " << (best_known.loaded ? best_known_str : "") << "\n";
   std::cout << "Best found obj = " << std::fixed << std::setprecision(9) << best_output_obj << "\n";
   std::cout << "Average obj = " << std::fixed << std::setprecision(9) << average_obj << "\n";
   std::cout << "Average TTS = " << average_tts_str << "\n";
   std::cout << "Times reaching best-known obj = " << reached_count << "\n";
   std::cout << "Summary appended to " << cfg.report_csv << "\n";

   if (!best_run_res.best_x.empty()) {
      double recomputed_obj = objective_x(best_run_res.best_x, ins);
      double diff = recomputed_obj - best_run_res.best_obj;
      if (std::abs(diff) > 1e-5) {
         std::cout << "Wrong obj value !!\n";
      }

      std::cout << "Selected vars of best run: ";
      for (int i = 0; i < ins.n; i++) {
         if (best_run_res.best_x[i] == 1) {
            std::cout << (i + 1) << " ";
         }
      }
      std::cout << "\n";
   }

   return 0;
}

static bool is_instance_file(const fs::path& path) {
   std::string ext = path.extension().string();
   std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
   });
   return ext == ".qsc" || ext == ".txt";
}

static void append_batch_configs(
   std::vector<RunConfig>& configs,
   const std::string& dataset_dir,
   const std::string& set_name,
   ObjectiveSense sense,
   PrintScale scale,
   const std::string& best_known_arg,
   uint64_t base_seed,
   int num_runs,
   const std::string& report_csv
) {
   const fs::path set_root = fs::path(dataset_dir) / set_name;
   std::error_code ec;
   if (!fs::exists(set_root, ec) || !fs::is_directory(set_root, ec)) {
      std::cerr << "WARNING: dataset set not found: " << set_root.string() << "\n";
      return;
   }

   std::vector<fs::path> files;
   for (fs::recursive_directory_iterator it(set_root, ec), end; !ec && it != end; it.increment(ec)) {
      if (it->is_regular_file(ec) && is_instance_file(it->path())) {
         files.push_back(it->path());
      }
   }
   if (ec) {
      throw std::runtime_error("Failed while scanning dataset set: " + set_root.string());
   }

   std::sort(files.begin(), files.end());
   for (const fs::path& file : files) {
      RunConfig cfg;
      cfg.sense = sense;
      cfg.scale = scale;
      cfg.instance_path = file.string();
      cfg.best_known_arg = best_known_arg;
      cfg.base_seed = base_seed + static_cast<uint64_t>(configs.size()) * 0x9E3779B97F4A7C15ULL;
      cfg.num_runs = num_runs;
      cfg.report_csv = report_csv;
      configs.push_back(cfg);
   }
}

static int run_batch(
   const std::string& dataset_dir,
   const std::string& best_known_arg,
   uint64_t base_seed,
   int num_runs,
   const std::string& report_csv
) {
   if (num_runs <= 0) {
      throw std::runtime_error("Number of runs must be positive.");
   }

   std::vector<RunConfig> configs;
   append_batch_configs(configs, dataset_dir, "MDP", ObjectiveSense::Max, PrintScale::Half, best_known_arg, base_seed, num_runs, report_csv);
   append_batch_configs(configs, dataset_dir, "MCP", ObjectiveSense::Max, PrintScale::Full, best_known_arg, base_seed, num_runs, report_csv);
   append_batch_configs(configs, dataset_dir, "GBP", ObjectiveSense::Min, PrintScale::Full, best_known_arg, base_seed, num_runs, report_csv);

   if (configs.empty()) {
      throw std::runtime_error("No .qsc or .txt instances were found under: " + dataset_dir);
   }

   std::cout << "Batch mode: " << configs.size() << " instances\n";
   std::cout << "Dataset folder: " << dataset_dir << "\n";
   std::cout << "Best-known CSV: " << best_known_arg << "\n";
   std::cout << "Summary file: " << report_csv << "\n";

   int failures = 0;
   for (size_t idx = 0; idx < configs.size(); ++idx) {
      std::cout << "\n=== Instance " << (idx + 1) << "/" << configs.size() << " ===\n";
      try {
         run_single_instance(configs[idx]);
      }
      catch (const std::exception& e) {
         ++failures;
         std::cerr << "ERROR: failed instance " << configs[idx].instance_path << ": " << e.what() << "\n";
      }
   }

   if (failures > 0) {
      std::cerr << "Batch completed with " << failures << " failed instance(s).\n";
      return 1;
   }

   std::cout << "Batch completed successfully.\n";
   return 0;
}

static void print_usage(const char* exe) {
   std::cerr << "Usage:\n"
             << "  " << exe << "\n"
             << "  " << exe << " batch [dataset_dir] [best_known_results.csv] [base_seed] [runs] [report.csv]\n"
             << "  " << exe << " <max|min> <full|half> <instance.qsc_or_txt> [best_known_results.csv_or_folder] [base_seed] [runs] [report.csv]\n\n"
             << "Default batch settings: MDP=max half, MCP=max full, GBP=min full.\n"
             << "If [report.csv] is omitted, FCBQP_10runs_summary.csv is written to the same directory as the Slurm .out log file when possible.\n";
}

int main(int argc, char* argv[]) {
   try {
      if (argc == 1 || (argc >= 2 && std::string(argv[1]) == "batch")) {
         const int offset = (argc >= 2 && std::string(argv[1]) == "batch") ? 1 : 0;
         const std::string dataset_dir = (argc >= offset + 2 ? std::string(argv[offset + 1]) : "dataset");
         const std::string best_known_arg = (argc >= offset + 3 ? std::string(argv[offset + 2]) : "best_known_results.csv");
         const uint64_t base_seed = (argc >= offset + 4 ? std::stoull(argv[offset + 3]) : 1ULL);
         const int num_runs = (argc >= offset + 5 ? std::stoi(argv[offset + 4]) : 10);
         const std::string report_csv = (argc >= offset + 6 ? std::string(argv[offset + 5]) : default_report_csv_path());
         return run_batch(dataset_dir, best_known_arg, base_seed, num_runs, report_csv);
      }

      if (argc < 4) {
         print_usage(argv[0]);
         return 1;
      }

      RunConfig cfg;
      cfg.sense = parse_objective_sense(argv[1]);
      cfg.scale = parse_print_scale(argv[2]);
      cfg.instance_path = argv[3];
      cfg.best_known_arg = (argc >= 5 ? std::string(argv[4]) : "best_known_results.csv");
      cfg.base_seed = (argc >= 6 ? std::stoull(argv[5]) : 1ULL);
      cfg.num_runs = (argc >= 7 ? std::stoi(argv[6]) : 10);
      cfg.report_csv = (argc >= 8 ? std::string(argv[7]) : default_report_csv_path());
      return run_single_instance(cfg);
   }
   catch (const std::exception& e) {
      std::cerr << "ERROR: " << e.what() << "\n";
      return 1;
   }
}
