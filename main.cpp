#include <algorithm>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <execution>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <numeric>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

struct Instance {
   int n = 0;
   int k = 0;
   double objective_offset = 0.0;
   bool sparse = false;
   double density = 0.0;
   size_t nnz_full = 0;
   std::vector<double> Q;
   std::vector<int> row_ptr;
   std::vector<int> col_ind;
   std::vector<double> val;
   std::vector<double> diagonal;
};

static constexpr double SPARSE_DENSITY_THRESHOLD = 0.002;
static constexpr size_t MAX_DENSE_MATRIX_BYTES = 512ULL * 1024ULL * 1024ULL;

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
   int epoch_lanes_per_worker = 1;
   int elite_group_size = std::numeric_limits<int>::max();
   int construction_max_attempts = 200;
   double d_frac = 0.2;
   double time_limit_seconds = 600.0;
};

struct Result {
   std::vector<int> best_x;
   double best_obj = std::numeric_limits<double>::infinity();
   double time_to_best = 0.0;
   double time_to_target = std::numeric_limits<double>::quiet_NaN();
   int start_to_best = 0;
   int target_epoch = 0;
   bool reached_target = false;
};

struct EliteSol {
   std::vector<int> x;
   std::vector<int> ones;
   std::vector<uint64_t> bits;
   double obj = std::numeric_limits<double>::infinity();
   int min_hamming = std::numeric_limits<int>::max();
};

struct EliteState {
   std::vector<EliteSol> pool;
   std::vector<int> ones_count;
   std::vector<int> construction_candidates;
};

static inline std::string trim(const std::string& s) {
   const size_t a = s.find_first_not_of(" \t\r\n");
   if (a == std::string::npos) return "";
   const size_t b = s.find_last_not_of(" \t\r\n");
   return s.substr(a, b - a + 1);
}

static inline void skip_ascii_space(const char*& cursor, const char* end) {
   while (cursor != end && (*cursor == ' ' || *cursor == '\t' || *cursor == '\r')) ++cursor;
}

static bool parse_int_field(const char*& cursor, const char* end, int& value) {
   skip_ascii_space(cursor, end);
   const std::from_chars_result parsed = std::from_chars(cursor, end, value);
   if (parsed.ec != std::errc()) return false;
   cursor = parsed.ptr;
   return true;
}

static bool parse_matrix_entry(const std::string& line, int& i, int& j, double& value) {
   const char* cursor = line.data();
   const char* end = cursor + line.size();
   if (!parse_int_field(cursor, end, i) || !parse_int_field(cursor, end, j)) return false;
   skip_ascii_space(cursor, end);
   if (cursor == end) return false;

   char* value_end = nullptr;
   value = std::strtod(cursor, &value_end);
   if (value_end == cursor) return false;
   cursor = value_end;
   skip_ascii_space(cursor, end);
   return cursor == end;
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

struct DeterministicTargetControl {
   std::atomic<uint64_t> logical_cutoff{ std::numeric_limits<uint64_t>::max() };

   void publish(uint64_t logical_work) {
      uint64_t current = logical_cutoff.load(std::memory_order_relaxed);
      while (logical_work < current &&
         !logical_cutoff.compare_exchange_weak(
            current,
            logical_work,
            std::memory_order_release,
            std::memory_order_relaxed
         )) {
      }
   }
};

struct LogicalWorkContext {
   DeterministicTargetControl* target = nullptr;
   uint64_t* epoch_work = nullptr;
};

static thread_local LogicalWorkContext* active_logical_work = nullptr;

class LogicalWorkScope {
public:
   explicit LogicalWorkScope(LogicalWorkContext& context)
      : previous_(active_logical_work) {
      active_logical_work = &context;
   }

   ~LogicalWorkScope() { active_logical_work = previous_; }

   LogicalWorkScope(const LogicalWorkScope&) = delete;
   LogicalWorkScope& operator=(const LogicalWorkScope&) = delete;

private:
   LogicalWorkContext* previous_;
};

struct PhaseProfile {
   double parallel_seconds = 0.0;
   double greedy_seconds = 0.0;
   double diversity_seconds = 0.0;
   double tabu_seconds = 0.0;
   double tabu_prefix_seconds = 0.0;
   double sparse_move_seconds = 0.0;
   double path_seconds = 0.0;
   double elite_merge_seconds = 0.0;
   uint64_t greedy_calls = 0;
   uint64_t greedy_attempts = 0;
   uint64_t greedy_additions = 0;
   uint64_t tabu_iterations = 0;
   uint64_t path_steps = 0;

   PhaseProfile& operator+=(const PhaseProfile& other) {
      parallel_seconds += other.parallel_seconds;
      greedy_seconds += other.greedy_seconds;
      diversity_seconds += other.diversity_seconds;
      tabu_seconds += other.tabu_seconds;
      tabu_prefix_seconds += other.tabu_prefix_seconds;
      sparse_move_seconds += other.sparse_move_seconds;
      path_seconds += other.path_seconds;
      elite_merge_seconds += other.elite_merge_seconds;
      greedy_calls += other.greedy_calls;
      greedy_attempts += other.greedy_attempts;
      greedy_additions += other.greedy_additions;
      tabu_iterations += other.tabu_iterations;
      path_steps += other.path_steps;
      return *this;
   }
};

#if defined(FCBQP_ENABLE_PHASE_PROFILING)
static thread_local PhaseProfile* active_phase_profile = nullptr;

class PhaseProfileScope {
public:
   explicit PhaseProfileScope(PhaseProfile& profile)
      : previous_(active_phase_profile) {
      active_phase_profile = &profile;
   }

   ~PhaseProfileScope() { active_phase_profile = previous_; }

   PhaseProfileScope(const PhaseProfileScope&) = delete;
   PhaseProfileScope& operator=(const PhaseProfileScope&) = delete;

private:
   PhaseProfile* previous_;
};

class PhaseTimer {
public:
   explicit PhaseTimer(double PhaseProfile::* member)
      : profile_(active_phase_profile), member_(member) {
      if (profile_ != nullptr) start_ = std::chrono::steady_clock::now();
   }

   ~PhaseTimer() {
      if (profile_ == nullptr) return;
      const auto stop = std::chrono::steady_clock::now();
      profile_->*member_ += std::chrono::duration<double>(stop - start_).count();
   }

   PhaseTimer(const PhaseTimer&) = delete;
   PhaseTimer& operator=(const PhaseTimer&) = delete;

private:
   PhaseProfile* profile_;
   double PhaseProfile::* member_;
   std::chrono::steady_clock::time_point start_{};
};

#define FCBQP_PROFILE_TIMER(name, member) \
   PhaseTimer name(&PhaseProfile::member)
#define FCBQP_PROFILE_INCREMENT(member) \
   do { if (active_phase_profile != nullptr) ++active_phase_profile->member; } while (false)
#else
#define FCBQP_PROFILE_TIMER(name, member)
#define FCBQP_PROFILE_INCREMENT(member) do { } while (false)
#endif

void account_logical_work(uint64_t amount = 1) {
   if (active_logical_work == nullptr || amount == 0) return;
   *active_logical_work->epoch_work += amount;
}

void publish_deterministic_target() {
   if (active_logical_work == nullptr) return;
   active_logical_work->target->publish(*active_logical_work->epoch_work);
}

struct SearchControl {
   const std::chrono::steady_clock::time_point& t_start;
   double time_limit_seconds;
   std::atomic<bool>& stop_requested;

   bool should_stop() const {
      if (stop_requested.load(std::memory_order_relaxed)) return true;
      if (!time_limit_reached(t_start, time_limit_seconds)) return false;
      stop_requested.store(true, std::memory_order_relaxed);
      return true;
   }
};

int parse_worker_count(const char* text, const char* source) {
   if (text == nullptr || *text == '\0') {
      throw std::runtime_error(std::string(source) + " must be a positive integer.");
   }
   char* end = nullptr;
   const long value = std::strtol(text, &end, 10);
   if (end == text || *end != '\0' || value <= 0 || value > std::numeric_limits<int>::max()) {
      throw std::runtime_error(std::string(source) + " must be a positive integer.");
   }
   return static_cast<int>(value);
}

int default_worker_count() {
   if (const char* configured = std::getenv("FCBQP_NUM_THREADS")) {
      return parse_worker_count(configured, "FCBQP_NUM_THREADS");
   }
   return 4;
}

void apply_schedule_test_delay(int lane_id) {
   static const int delayed_lane = [] {
      const char* value = std::getenv("FCBQP_TEST_DELAY_LANE");
      return value == nullptr ? -1 : std::atoi(value);
      }();
   static const int delay_microseconds = [] {
      const char* value = std::getenv("FCBQP_TEST_DELAY_US");
      return value == nullptr ? 0 : std::max(0, std::atoi(value));
      }();
   if (lane_id == delayed_lane && delay_microseconds > 0) {
      std::this_thread::sleep_for(std::chrono::microseconds(delay_microseconds));
   }
}

const char* parallel_backend_name() {
#if defined(_LIBCPP_PSTL_BACKEND_LIBDISPATCH)
   return "C++20 PSTL/libdispatch";
#elif defined(_PSTL_PAR_BACKEND_TBB)
   return "C++20 PSTL/TBB";
#elif defined(_PSTL_PAR_BACKEND_OPENMP)
   return "C++20 PSTL/OpenMP";
#elif defined(_PSTL_PAR_BACKEND_SERIAL)
   return "C++20 PSTL/serial";
#else
   return "C++20 PSTL/compiler-default";
#endif
}

template <class Function>
void parallel_for_workers(int worker_count, Function&& function) {
   if (worker_count <= 1) {
      function(0);
      return;
   }

   constexpr size_t tickets_per_worker = 256;
   const size_t ticket_count = std::max<size_t>(1024, static_cast<size_t>(worker_count) * tickets_per_worker);
   static thread_local std::vector<unsigned char> tickets;
   tickets.resize(ticket_count);
   std::atomic<int> next_worker{ 0 };
   std::for_each(std::execution::par, tickets.begin(), tickets.end(), [&](unsigned char) noexcept {
      int worker_id = next_worker.load(std::memory_order_relaxed);
      while (worker_id < worker_count &&
         !next_worker.compare_exchange_weak(
            worker_id,
            worker_id + 1,
            std::memory_order_relaxed,
            std::memory_order_relaxed
         )) {
      }
      if (worker_id < worker_count) function(worker_id);
      });
}

using DeterministicRng = std::mt19937_64;

uint64_t bounded_random(DeterministicRng& rng, uint64_t bound) {
   if (bound == 0) {
      throw std::logic_error("bounded_random requires a positive bound.");
   }
   if (bound == 1) return 0;

   const unsigned bits = std::bit_width(bound - 1);
   const uint64_t mask = (bits == 64U)
      ? std::numeric_limits<uint64_t>::max()
      : ((uint64_t{ 1 } << bits) - 1U);
   uint64_t value = 0;
   do {
      value = rng() & mask;
   } while (value >= bound);
   return value;
}

template <class RandomAccessIterator>
void deterministic_shuffle(RandomAccessIterator first, RandomAccessIterator last, DeterministicRng& rng) {
   using Difference = typename std::iterator_traits<RandomAccessIterator>::difference_type;
   const Difference size = last - first;
   if (size <= 1) return;
   Difference remaining = size - 1;
   for (Difference position = 0; position < size - 1; ++position, --remaining) {
      const Difference offset = static_cast<Difference>(
         bounded_random(rng, static_cast<uint64_t>(remaining + 1))
      );
      if (offset != 0) std::iter_swap(first + position, first + position + offset);
   }
}

static std::string basename_without_extension(const std::string& path) {
   size_t slash = path.find_last_of("/\\");
   std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
   size_t dot = name.find_last_of('.');
   if (dot != std::string::npos) name.resize(dot);
   return name;
}

static bool regular_file_exists(const std::string& path) {
   namespace fs = std::filesystem;
   std::error_code ec;
   return fs::exists(path, ec) && fs::is_regular_file(path, ec);
}

static bool directory_exists(const std::string& path) {
   namespace fs = std::filesystem;
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

   throw std::runtime_error("Objective sense must be either 'max' or 'min'.");
}

PrintScale parse_print_scale(const std::string& s) {
   std::string t = s;
   std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
      });

   if (t == "full") return PrintScale::Full;
   if (t == "half") return PrintScale::Half;

   throw std::runtime_error("Print scale must be either 'full' or 'half'.");
}

static inline const char* objective_sense_name(ObjectiveSense sense) {
   return (sense == ObjectiveSense::Max) ? "max" : "min";
}

static inline const char* print_scale_name(PrintScale scale) {
   return (scale == PrintScale::Full) ? "full" : "half";
}

static inline double output_objective_from_internal(
   double internal_obj,
   double internal_offset,
   ObjectiveSense sense,
   PrintScale scale
) {
   const double adjusted_obj = internal_obj + internal_offset;

   if (sense == ObjectiveSense::Max) {
      return (scale == PrintScale::Full)
         ? (-1.0) * adjusted_obj
         : (-0.5) * adjusted_obj;
   }

   return (scale == PrintScale::Full)
      ? adjusted_obj
      : 0.5 * adjusted_obj;
}

static inline bool target_reached(
   double output_obj,
   double target_obj,
   ObjectiveSense sense,
   double tol = 1e-6
) {
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
      if (endptr == obj_str.c_str() || *endptr != '\0' || !std::isfinite(best_obj)) {
         std::cerr << "WARNING: skipping CSV line " << line_no
            << " because best_known_obj is not a finite number: "
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

static BestKnownEntry find_best_known_for_instance(
   const std::string& csv_filename,
   const std::string& instance_path
) {
   if (csv_filename.empty()) {
      return BestKnownEntry{};
   }
   const auto table = read_best_known_csv(csv_filename);
   const std::string base = basename_without_extension(instance_path);
   auto it = table.find(base);
   if (it != table.end()) return it->second;
   const std::string name_with_ext = std::filesystem::path(instance_path).filename().string();
   it = table.find(name_with_ext);
   if (it != table.end()) return it->second;
   return BestKnownEntry{};
}

using BestKnownObjective = BestKnownEntry;

static std::string resolve_best_known_objective_path(
   const std::string& instance_path,
   const std::string& best_known_arg
) {
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

static BestKnownObjective read_best_known_objective_file(
   const std::string& csv_filename,
   const std::string& instance_path
) {
   BestKnownObjective e = find_best_known_for_instance(csv_filename, instance_path);
   if (e.loaded) {
      e.filename = csv_filename;
      if (e.matched_instance.empty()) e.matched_instance = e.instance;
   }
   return e;
}

Instance read_instance(
   const std::string& filename,
   ObjectiveSense sense,
   std::optional<int> forced_index_base = std::nullopt
) {
   std::ifstream in(filename);
   if (!in) {
      throw std::runtime_error("Cannot open file: " + filename);
   }

   std::string line;
   int source_line = 0;
   auto next_data_line = [&]() -> bool {
      while (std::getline(in, line)) {
         ++source_line;
         line = trim(line);
         if (line.empty() || line.front() == '#') continue;
         return true;
      }
      return false;
   };

   if (!next_data_line()) {
      throw std::runtime_error("File has no non-comment lines.");
   }

   struct Entry {
      int i, j;
      double v;
   };
   struct UndirectedEntry {
      int a, b;
      bool forward;
      double v;
   };
   Instance ins;
   int declared_num_entries = -1;
   int detected_type = 0;
   std::vector<std::string> first_tokens;
   {
      std::istringstream iss(line);
      std::string tok;
      while (iss >> tok) {
         first_tokens.push_back(tok);
      }
   }
   if (first_tokens.size() == 1) {
      {
         std::istringstream iss(line);
         if (!(iss >> ins.k)) {
            throw std::runtime_error("Failed to read k from first line.");
         }
      }
      if (!next_data_line()) {
         throw std::runtime_error("Type 2 data file needs at least two header lines.");
      }
      {
         std::istringstream iss(line);
         if (!(iss >> ins.n >> declared_num_entries)) {
            throw std::runtime_error("Failed to read n and number_of_entries from second line for Type 2 format.");
         }
      }
      detected_type = 2;
   }
   else if (first_tokens.size() == 2) {
      std::istringstream iss(line);
      if (!(iss >> ins.n >> ins.k)) {
         throw std::runtime_error("Failed to read n and k from first line for Type 1 format.");
      }
      declared_num_entries = -1;
      detected_type = 1;
   }
   else if (first_tokens.size() == 3) {
      std::istringstream iss(line);
      if (!(iss >> ins.n >> ins.k >> declared_num_entries)) {
         throw std::runtime_error("Failed to read n, k, and number_of_entries from first line for Type 3 format.");
      }
      detected_type = 3;
   }
   else {
      throw std::runtime_error("Cannot detect data format.");
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

   std::vector<UndirectedEntry> entries;
   if (declared_num_entries > 0) {
      entries.reserve(static_cast<size_t>(declared_num_entries));
   }

   int min_index = std::numeric_limits<int>::max();
   int max_index = std::numeric_limits<int>::min();

   while (next_data_line()) {
      int i, j;
      double v;

      if (!parse_matrix_entry(line, i, j, v)) {
         throw std::runtime_error(
            "Invalid matrix entry on line " + std::to_string(source_line) + ": " + line
         );
      }
      if (!std::isfinite(v)) {
         throw std::runtime_error(
            "Non-finite matrix value on line " + std::to_string(source_line) + "."
         );
      }

      entries.push_back({ i, j, false, v });

      min_index = std::min(min_index, std::min(i, j));
      max_index = std::max(max_index, std::max(i, j));
   }

   if (declared_num_entries >= 0 &&
      static_cast<int>(entries.size()) != declared_num_entries) {
      std::cerr << "WARNING: declared number_of_entries = "
         << declared_num_entries
         << ", but actually read "
         << entries.size()
         << " entries.\n";
   }
   const size_t input_entry_count = entries.size();

   int shift = 0;

   if (!entries.empty()) {
      const bool valid_zero_based =
         (min_index >= 0 && max_index <= ins.n - 1);

      const bool valid_one_based =
         (min_index >= 1 && max_index <= ins.n);

      if (forced_index_base.has_value()) {
         if (*forced_index_base != 0 && *forced_index_base != 1) {
            throw std::runtime_error("Forced index base must be 0 or 1.");
         }
         const bool valid_forced = (*forced_index_base == 0) ? valid_zero_based : valid_one_based;
         if (!valid_forced) {
            throw std::runtime_error("The requested index base is incompatible with the indices in the file.");
         }
         shift = *forced_index_base;
      }
      else if (valid_zero_based && !valid_one_based) {
         shift = 0;
      }
      else if (!valid_zero_based && valid_one_based) {
         shift = 1;
      }
      else if (valid_zero_based && valid_one_based) {
         throw std::runtime_error(
            "Index base is ambiguous; pass an explicit final argument of 0 or 1."
         );
      }
      else {
         std::ostringstream oss;
         oss << "Cannot detect index base. Index range in file is ["
            << min_index << ", " << max_index
            << "], but n = " << ins.n << ".";
         throw std::runtime_error(oss.str());
      }
   }

   for (auto& e : entries) {
      const int raw_i = e.a;
      const int raw_j = e.b;
      const int i = raw_i - shift;
      const int j = raw_j - shift;

      if (i < 0 || i >= ins.n || j < 0 || j >= ins.n) {
         std::ostringstream oss;
         oss << "Index out of range while reading file. "
            << "Original entry: (" << raw_i << ", " << raw_j << "), "
            << "after shift: (" << i << ", " << j << "), "
            << "n = " << ins.n << ".";
         throw std::runtime_error(oss.str());
      }

      e.a = std::min(i, j);
      e.b = std::max(i, j);
      e.forward = (i <= j);
   }

   const auto undirected_less = [](const UndirectedEntry& lhs, const UndirectedEntry& rhs) {
      if (lhs.a != rhs.a) return lhs.a < rhs.a;
      if (lhs.b != rhs.b) return lhs.b < rhs.b;
      if (lhs.forward != rhs.forward) return lhs.forward < rhs.forward;
      return lhs.v < rhs.v;
      };
   if (!std::is_sorted(entries.begin(), entries.end(), undirected_less)) {
      std::sort(entries.begin(), entries.end(), undirected_less);
   }

   std::vector<Entry> canonical;
   canonical.reserve(entries.size());
   size_t symmetric_pairs = 0;
   size_t nnz_full = 0;
   for (size_t p = 0; p < entries.size();) {
      const int a = entries[p].a;
      const int b = entries[p].b;
      double forward = 0.0;
      double reverse = 0.0;
      bool have_forward = false;
      bool have_reverse = false;
      size_t q = p;
      while (q < entries.size() && entries[q].a == a && entries[q].b == b) {
         if (entries[q].forward) {
            forward += entries[q].v;
            have_forward = true;
         }
         else {
            reverse += entries[q].v;
            have_reverse = true;
         }
         ++q;
      }

      double value = forward + reverse;
      if (a != b && have_forward && have_reverse) {
         const double tolerance = 1e-9 * std::max({ 1.0, std::abs(forward), std::abs(reverse) });
         if (std::abs(forward - reverse) > tolerance) {
            std::ostringstream oss;
            oss << "Input matrix is not symmetric at (" << (a + shift) << ", " << (b + shift)
               << "): " << forward << " versus " << reverse << ".";
            throw std::runtime_error(oss.str());
         }
         value = 0.5 * (forward + reverse);
         ++symmetric_pairs;
      }

      const double internal_value = (sense == ObjectiveSense::Max) ? -value : value;
      if (internal_value != 0.0) {
         canonical.push_back({ a, b, internal_value });
         nnz_full += (a == b ? 1U : 2U);
      }
      p = q;
   }

   entries.clear();
   entries.shrink_to_fit();

   ins.nnz_full = nnz_full;
   const double denom = static_cast<double>(ins.n) * static_cast<double>(ins.n);
   ins.density = (denom > 0.0) ? static_cast<double>(ins.nnz_full) / denom : 0.0;
   const long double dense_bytes = static_cast<long double>(ins.n) * static_cast<long double>(ins.n) * sizeof(double);
   const bool dense_size_allowed = dense_bytes <= static_cast<long double>(MAX_DENSE_MATRIX_BYTES);
   ins.sparse = (ins.density <= SPARSE_DENSITY_THRESHOLD) || !dense_size_allowed;
   ins.diagonal.assign(static_cast<size_t>(ins.n), 0.0);

   if (ins.sparse) {
      if (ins.nnz_full > static_cast<size_t>(std::numeric_limits<int>::max())) {
         throw std::runtime_error("Sparse matrix has too many entries for 32-bit CSR indices.");
      }
      ins.Q.clear();
      ins.row_ptr.assign(static_cast<size_t>(ins.n) + 1, 0);
      ins.col_ind.resize(ins.nnz_full);
      ins.val.resize(ins.nnz_full);

      for (const auto& e : canonical) {
         ins.row_ptr[static_cast<size_t>(e.i) + 1]++;
         if (e.i != e.j) ins.row_ptr[static_cast<size_t>(e.j) + 1]++;
      }

      for (int i = 0; i < ins.n; ++i) {
         ins.row_ptr[i + 1] += ins.row_ptr[i];
      }

      std::vector<int> next = ins.row_ptr;
      for (const auto& e : canonical) {
         const int pos = next[e.i]++;
         ins.col_ind[pos] = e.j;
         ins.val[pos] = e.v;
         if (e.i == e.j) {
            ins.diagonal[e.i] = e.v;
         }
         else {
            const int reverse_pos = next[e.j]++;
            ins.col_ind[reverse_pos] = e.i;
            ins.val[reverse_pos] = e.v;
         }
      }
   }
   else {
      ins.row_ptr.clear();
      ins.col_ind.clear();
      ins.val.clear();
      ins.Q.assign(static_cast<size_t>(ins.n) * static_cast<size_t>(ins.n), 0.0);
      for (const auto& e : canonical) {
         Qref(ins, e.i, e.j) = e.v;
         Qref(ins, e.j, e.i) = e.v;
         if (e.i == e.j) ins.diagonal[e.i] = e.v;
      }
   }

   std::cerr << "Data type = " << detected_type
      << ", n = " << ins.n
      << ", k = " << ins.k
      << ", input entries = " << input_entry_count
      << ", full nnz = " << ins.nnz_full
      << ", density = " << std::fixed << std::setprecision(6) << (100.0 * ins.density) << "%"
      << ", storage = " << (ins.sparse ? "CSR sparse" : "dense matrix")
      << ", symmetric pairs supplied twice = " << symmetric_pairs
      << ", index base = " << (shift == 0 ? 0 : 1)
      << "-based\n";

   return ins;
}

void invert_instance(Instance& ins) {
   const int n = ins.n;
   std::vector<double> row_sums(static_cast<size_t>(n), 0.0);
   double offset_increment = 0.0;

   for (int i = 0; i < n; ++i) {
      double row_sum = 0.0;
      if (ins.sparse) {
         for (int p = ins.row_ptr[i]; p < ins.row_ptr[i + 1]; ++p) {
            row_sum += ins.val[p];
            if (!std::isfinite(row_sum)) {
               throw std::runtime_error("Inversion overflowed while summing a sparse row.");
            }
         }
      }
      else {
         const double* row = &ins.Q[static_cast<size_t>(i) * static_cast<size_t>(n)];
         for (int j = 0; j < n; ++j) {
            row_sum += row[j];
            if (!std::isfinite(row_sum)) {
               throw std::runtime_error("Inversion overflowed while summing a dense row.");
            }
         }
      }
      row_sums[i] = row_sum;
      offset_increment += row_sum;
      if (!std::isfinite(offset_increment)) {
         throw std::runtime_error("Inversion objective offset is not finite.");
      }
   }

   std::vector<double> new_diagonal(static_cast<size_t>(n));
   for (int i = 0; i < n; ++i) {
      const double value = ins.diagonal[i] - 2.0 * row_sums[i];
      if (!std::isfinite(value)) {
         throw std::runtime_error("Inversion produced a non-finite diagonal coefficient.");
      }
      new_diagonal[i] = value;
   }

   size_t new_nnz_full = ins.nnz_full;
   if (ins.sparse) {
      std::vector<int> new_row_ptr(static_cast<size_t>(n) + 1, 0);
      new_nnz_full = 0;
      for (int i = 0; i < n; ++i) {
         for (int p = ins.row_ptr[i]; p < ins.row_ptr[i + 1]; ++p) {
            if (ins.col_ind[p] != i) ++new_nnz_full;
         }
         if (new_diagonal[i] != 0.0) ++new_nnz_full;
         if (new_nnz_full > static_cast<size_t>(std::numeric_limits<int>::max())) {
            throw std::runtime_error(
               "Inverted sparse matrix has too many entries for 32-bit CSR indices."
            );
         }
         new_row_ptr[static_cast<size_t>(i) + 1] = static_cast<int>(new_nnz_full);
      }

      std::vector<int> new_col_ind(new_nnz_full);
      std::vector<double> new_val(new_nnz_full);
      for (int i = 0; i < n; ++i) {
         int write = new_row_ptr[i];
         bool diagonal_written = false;
         for (int p = ins.row_ptr[i]; p < ins.row_ptr[i + 1]; ++p) {
            const int col = ins.col_ind[p];
            if (col == i) continue;
            if (!diagonal_written && new_diagonal[i] != 0.0 && col > i) {
               new_col_ind[write] = i;
               new_val[write++] = new_diagonal[i];
               diagonal_written = true;
            }
            new_col_ind[write] = col;
            new_val[write++] = ins.val[p];
         }
         if (!diagonal_written && new_diagonal[i] != 0.0) {
            new_col_ind[write] = i;
            new_val[write++] = new_diagonal[i];
         }
         if (write != new_row_ptr[static_cast<size_t>(i) + 1]) {
            throw std::logic_error("Inversion built an inconsistent CSR row.");
         }
      }
      ins.row_ptr = std::move(new_row_ptr);
      ins.col_ind = std::move(new_col_ind);
      ins.val = std::move(new_val);
   }
   else {
      for (int i = 0; i < n; ++i) {
         const bool had_diagonal = ins.diagonal[i] != 0.0;
         const bool has_diagonal = new_diagonal[i] != 0.0;
         if (had_diagonal && !has_diagonal) --new_nnz_full;
         if (!had_diagonal && has_diagonal) ++new_nnz_full;
         Qref(ins, i, i) = new_diagonal[i];
      }
   }

   const double new_offset = ins.objective_offset + offset_increment;
   if (!std::isfinite(new_offset)) {
      throw std::runtime_error("Inversion accumulated a non-finite objective offset.");
   }
   ins.objective_offset = new_offset;
   ins.k = n - ins.k;
   ins.nnz_full = new_nnz_full;
   ins.diagonal = std::move(new_diagonal);
   const double denom = static_cast<double>(n) * static_cast<double>(n);
   ins.density = (denom > 0.0)
      ? static_cast<double>(ins.nnz_full) / denom
      : 0.0;
}

bool canonical_orientation_requires_inversion(const Instance& ins) {
   return ins.k > ins.n - ins.k;
}
bool canonicalize_instance(Instance& ins) {
   const bool inverted = canonical_orientation_requires_inversion(ins);
   if (inverted) invert_instance(ins);
   return inverted;
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

   std::vector<int> selected;
   selected.reserve(static_cast<size_t>(ins.k));
   for (int i = 0; i < n; ++i) {
      if (x[i]) selected.push_back(i);
   }
   for (size_t a = 0; a < selected.size(); ++a) {
      const int i = selected[a];
      obj += Qat(ins, i, i);
      for (size_t b = a + 1; b < selected.size(); ++b) {
         obj += 2.0 * Qat(ins, i, selected[b]);
      }
   }
   return obj;
}

void compute_h_x_into(
   const std::vector<int>& x,
   const Instance& ins,
   std::vector<double>& h
) {
   const int n = static_cast<int>(x.size());
   h.assign(static_cast<size_t>(n), 0.0);

   for (int col = 0; col < n; ++col) {
      if (!x[col]) continue;
      if (ins.sparse) {
         for (int p = ins.row_ptr[col]; p < ins.row_ptr[col + 1]; ++p) {
            h[ins.col_ind[p]] += ins.val[p];
         }
      }
      else {
         const double* row = &ins.Q[static_cast<size_t>(col) * static_cast<size_t>(n)];
         std::transform(
            std::execution::unseq,
            h.begin(),
            h.end(),
            row,
            h.begin(),
            [](double current, double value) { return current + value; }
         );
      }
   }
}

std::vector<double> compute_h_x(const std::vector<int>& x, const Instance& ins) {
   std::vector<double> h;
   compute_h_x_into(x, ins, h);
   return h;
}

static inline void add_symmetric_column_to_h(
   std::vector<double>& h,
   const Instance& ins,
   int col,
   double scale
) {
   if (scale == 0.0) return;

   if (ins.sparse) {
      for (int p = ins.row_ptr[col]; p < ins.row_ptr[col + 1]; ++p) {
         h[ins.col_ind[p]] += scale * ins.val[p];
      }
      return;
   }

   const double* row = &ins.Q[static_cast<size_t>(col) * static_cast<size_t>(ins.n)];
   std::transform(
      std::execution::unseq,
      h.begin(),
      h.end(),
      row,
      h.begin(),
      [scale](double current, double value) { return current + scale * value; }
   );
}

const std::vector<double>& diagonal_values(const Instance& ins) {
   return ins.diagonal;
}

std::vector<double> interaction_impact(const Instance& ins, int requested_workers) {
   const int n = ins.n;
   std::vector<double> imp(n, 0.0);
   if (n == 0) return imp;

   const double scale = (n > 1) ? (2.0 * static_cast<double>(ins.k - 1) / static_cast<double>(n - 1)) : 0.0;

   const int worker_count = std::min(n, std::max(1, requested_workers));
   parallel_for_workers(worker_count, [&](int block) noexcept {
      const int first = static_cast<int>(static_cast<int64_t>(block) * n / worker_count);
      const int last = static_cast<int>(static_cast<int64_t>(block + 1) * n / worker_count);
      for (int i = first; i < last; ++i) {
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
      });
   return imp;
}


std::vector<int> build_set(
   const Instance& ins,
   const Params& par,
   const std::vector<double>& impact
) {

   std::vector<int> idx(ins.n);
   std::iota(idx.begin(), idx.end(), 0);

   std::sort(idx.begin(), idx.end(), [&](int a, int b) {
      if (impact[a] != impact[b]) return impact[a] < impact[b];
      return a < b;
      });

   int v_0 = static_cast<int>(std::ceil(par.v_0_frac * ins.n));
   v_0 = std::min(v_0, ins.k);

   std::vector<int> C;
   C.reserve(v_0);
   for (int t = 0; t < v_0; ++t) C.push_back(idx[t]);
   return C;
}

std::vector<int> selected_indices(const std::vector<int>& x) {
   std::vector<int> ones;
   ones.reserve(static_cast<size_t>(std::count(x.begin(), x.end(), 1)));
   for (int i = 0; i < static_cast<int>(x.size()); ++i) {
      if (x[i]) ones.push_back(i);
   }
   return ones;
}

uint64_t solution_fingerprint(const std::vector<int>& x) {
   uint64_t hash = 1469598103934665603ULL;
   for (size_t i = 0; i < x.size(); ++i) {
      if (x[i] == 0) continue;
      uint64_t value = static_cast<uint64_t>(i + 1);
      for (int byte = 0; byte < 8; ++byte) {
         hash ^= value & 0xffU;
         hash *= 1099511628211ULL;
         value >>= 8;
      }
   }
   hash ^= static_cast<uint64_t>(x.size());
   hash *= 1099511628211ULL;
   return hash;
}

EliteSol make_elite_solution(const std::vector<int>& x, double obj) {
   EliteSol elite;
   elite.x = x;
   elite.ones = selected_indices(x);
   elite.bits.assign((x.size() + 63U) / 64U, 0);
   for (int i : elite.ones) {
      elite.bits[static_cast<size_t>(i) / 64U] |= uint64_t{ 1 } << (i % 64);
   }
   elite.obj = obj;
   return elite;
}

std::vector<uint64_t> packed_solution(const std::vector<int>& x) {
   std::vector<uint64_t> bits((x.size() + 63U) / 64U, 0);
   for (size_t i = 0; i < x.size(); ++i) {
      if (x[i]) bits[i / 64U] |= uint64_t{ 1 } << (i % 64U);
   }
   return bits;
}

int hamming_distance_packed(
   const std::vector<uint64_t>& lhs,
   const std::vector<uint64_t>& rhs
) {
   if (lhs.size() != rhs.size()) {
      throw std::logic_error("Packed solutions have different sizes.");
   }
   int distance = 0;
   for (size_t word = 0; word < lhs.size(); ++word) {
      distance += std::popcount(lhs[word] ^ rhs[word]);
   }
   return distance;
}

int hamming_distance_to_elite(const std::vector<int>& x, const EliteSol& elite, const Instance& ins) {
   int intersection = 0;
   for (int i : elite.ones) intersection += x[i] != 0;
   return 2 * (ins.k - intersection);
}

int hamming_distance_elites(const EliteSol& a, const EliteSol& b, const Instance& ins) {
   (void)ins;
   return hamming_distance_packed(a.bits, b.bits);
}

void refresh_elite_min_distances(std::vector<EliteSol>& elite_pool, const Instance& ins) {
   for (auto& elite : elite_pool) elite.min_hamming = std::numeric_limits<int>::max();
   for (size_t a = 0; a < elite_pool.size(); ++a) {
      for (size_t b = a + 1; b < elite_pool.size(); ++b) {
         const int distance = hamming_distance_elites(elite_pool[a], elite_pool[b], ins);
         elite_pool[a].min_hamming = std::min(elite_pool[a].min_hamming, distance);
         elite_pool[b].min_hamming = std::min(elite_pool[b].min_hamming, distance);
      }
   }
}

void add_to_elite_count(
   const std::vector<int>& selected,
   std::vector<int>& elite_ones_count
) {
   for (int i : selected) elite_ones_count[i]++;
}

void remove_from_elite_count(
   const std::vector<int>& selected,
   std::vector<int>& elite_ones_count
) {
   for (int i : selected) elite_ones_count[i]--;
}

bool try_insert_elite(
   const Instance& ins,
   std::vector<EliteSol>& elite_pool,
   std::vector<int>& elite_ones_count,
   const std::vector<int>& x,
   double obj,
   const Params& par
) {
   const double EPS = 1e-6;

   for (const auto& e : elite_pool) {
      if (e.x == x) return false;
   }

   auto sort_elite_pool = [&]() {
      std::sort(elite_pool.begin(), elite_pool.end(),
         [](const EliteSol& a, const EliteSol& b) {
            if (a.obj != b.obj) return a.obj < b.obj;
            return a.x < b.x;
         });
      };

   const EliteSol candidate = make_elite_solution(x, obj);
   std::vector<int> candidate_distances(elite_pool.size(), std::numeric_limits<int>::max());
   for (size_t r = 0; r < elite_pool.size(); ++r) {
      candidate_distances[r] = hamming_distance_elites(candidate, elite_pool[r], ins);
   }

   auto min_hamming_excluding = [&](int excluded_idx) {
      int dmin = std::numeric_limits<int>::max();
      for (int r = 0; r < static_cast<int>(elite_pool.size()); ++r) {
         if (r == excluded_idx) continue;
         dmin = std::min(dmin, candidate_distances[r]);
      }
      return dmin;
      };

   const int d = static_cast<int>(std::ceil(par.d_frac * ins.k));

   const int relaxed_threshold = std::max(2, static_cast<int>(std::ceil(0.80 * d)));

   if (elite_pool.empty()) {
      elite_pool.push_back(candidate);
      add_to_elite_count(candidate.ones, elite_ones_count);
      return true;
   }

   sort_elite_pool();
   const double best_obj = elite_pool.front().obj;
   const bool new_global_best = (obj < best_obj - EPS);

   int min_dist = std::numeric_limits<int>::max();
   for (int distance : candidate_distances) {
      min_dist = std::min(min_dist, distance);
   }

   if (static_cast<int>(elite_pool.size()) < 30) {
      const bool enough_diversity = (min_dist >= d);
      const bool good_and_comparable_diversity =
         new_global_best && (min_dist >= relaxed_threshold);

      if (enough_diversity || good_and_comparable_diversity) {
         elite_pool.push_back(candidate);
         add_to_elite_count(candidate.ones, elite_ones_count);
         sort_elite_pool();
         refresh_elite_min_distances(elite_pool, ins);
         return true;
      }
      return false;
   }

   int replace_idx = -1;
   double best_replacement_key = std::numeric_limits<double>::infinity();

   for (int r = 0; r < static_cast<int>(elite_pool.size()); ++r) {
      const int cand_dist_to_others = min_hamming_excluding(r);
      const int old_dist_to_others = elite_pool[r].min_hamming;

      const int old_comparable_dist =
         (old_dist_to_others == std::numeric_limits<int>::max())
         ? d
         : static_cast<int>(std::floor(0.80 * old_dist_to_others));

      const bool full_diversity_after_replacement =
         (cand_dist_to_others >= d);

      const bool comparable_diversity_after_replacement =
         (cand_dist_to_others >= relaxed_threshold) &&
         (cand_dist_to_others >= old_comparable_dist);

      const bool better_than_this_member = (obj < elite_pool[r].obj - EPS);
      const bool acceptable_quality = (obj <= elite_pool.back().obj + EPS);

      const bool can_replace =
         (acceptable_quality && full_diversity_after_replacement) ||
         (better_than_this_member && comparable_diversity_after_replacement) ||
         (new_global_best && cand_dist_to_others >= relaxed_threshold);

      if (!can_replace) continue;

      const double quality_gain = elite_pool[r].obj - obj;
      const double replacement_key = -quality_gain - 1e-6 * cand_dist_to_others;

      if (replacement_key < best_replacement_key) {
         best_replacement_key = replacement_key;
         replace_idx = r;
      }
   }

   if (replace_idx >= 0) {
      remove_from_elite_count(elite_pool[replace_idx].ones, elite_ones_count);
      elite_pool[replace_idx] = candidate;
      add_to_elite_count(candidate.ones, elite_ones_count);
      sort_elite_pool();
      refresh_elite_min_distances(elite_pool, ins);
      return true;
   }
   return false;
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

template <class RandomAccessIterator, class Compare>
bool deterministic_sort3(
   RandomAccessIterator x,
   RandomAccessIterator y,
   RandomAccessIterator z,
   Compare compare
) {
   if (!compare(*y, *x)) {
      if (!compare(*z, *y)) return false;
      std::iter_swap(y, z);
      if (compare(*y, *x)) std::iter_swap(x, y);
      return true;
   }
   if (compare(*z, *y)) {
      std::iter_swap(x, z);
      return true;
   }
   std::iter_swap(x, y);
   if (compare(*z, *y)) std::iter_swap(y, z);
   return true;
}

template <class RandomAccessIterator, class Compare>
void deterministic_selection_sort(
   RandomAccessIterator first,
   RandomAccessIterator last,
   Compare compare
) {
   for (RandomAccessIterator current = first; current != last; ++current) {
      RandomAccessIterator best = current;
      for (RandomAccessIterator candidate = current + 1; candidate != last; ++candidate) {
         if (compare(*candidate, *best)) best = candidate;
      }
      if (best != current) std::iter_swap(current, best);
   }
}

template <class RandomAccessIterator, class Compare>
bool deterministic_nth_find_guard(
   RandomAccessIterator& lower,
   RandomAccessIterator& upper,
   RandomAccessIterator middle,
   Compare compare
) {
   while (true) {
      if (lower == --upper) return false;
      if (compare(*upper, *middle)) return true;
   }
}

template <class RandomAccessIterator, class Compare>
void deterministic_nth_element(
   RandomAccessIterator first,
   RandomAccessIterator nth,
   RandomAccessIterator last,
   Compare compare
) {
   constexpr typename std::iterator_traits<RandomAccessIterator>::difference_type small_limit = 7;
   while (true) {
      if (nth == last) return;
      const auto length = last - first;
      if (length <= 1) return;
      if (length == 2) {
         if (compare(*(last - 1), *first)) std::iter_swap(first, last - 1);
         return;
      }
      if (length == 3) {
         deterministic_sort3(first, first + 1, last - 1, compare);
         return;
      }
      if (length <= small_limit) {
         deterministic_selection_sort(first, last, compare);
         return;
      }

      RandomAccessIterator middle = first + length / 2;
      RandomAccessIterator last_element = last - 1;
      unsigned swaps = deterministic_sort3(first, middle, last_element, compare) ? 1U : 0U;
      RandomAccessIterator lower = first;
      RandomAccessIterator upper = last_element;

      if (!compare(*lower, *middle)) {
         if (deterministic_nth_find_guard(lower, upper, middle, compare)) {
            std::iter_swap(lower, upper);
            ++swaps;
         }
         else {
            ++lower;
            upper = last;
            if (!compare(*first, *--upper)) {
               while (true) {
                  if (lower == upper) return;
                  if (compare(*first, *lower)) {
                     std::iter_swap(lower, upper);
                     ++swaps;
                     ++lower;
                     break;
                  }
                  ++lower;
               }
            }
            if (lower == upper) return;
            while (true) {
               while (!compare(*first, *lower)) ++lower;
               do {
                  --upper;
               } while (compare(*first, *upper));
               if (lower >= upper) break;
               std::iter_swap(lower, upper);
               ++swaps;
               ++lower;
            }
            if (nth < lower) return;
            first = lower;
            continue;
         }
      }

      ++lower;
      if (lower < upper) {
         while (true) {
            while (compare(*lower, *middle)) ++lower;
            do {
               --upper;
            } while (!compare(*upper, *middle));
            if (lower >= upper) break;
            std::iter_swap(lower, upper);
            ++swaps;
            if (middle == lower) middle = upper;
            ++lower;
         }
      }
      if (lower != middle && compare(*middle, *lower)) {
         std::iter_swap(lower, middle);
         ++swaps;
      }
      if (nth == lower) return;

      if (swaps == 0) {
         if (nth < lower) {
            RandomAccessIterator previous = first;
            RandomAccessIterator current = first;
            while (true) {
               if (++current == lower) return;
               if (compare(*current, *previous)) break;
               previous = current;
            }
         }
         else {
            RandomAccessIterator previous = lower;
            RandomAccessIterator current = lower;
            while (true) {
               if (++current == last) return;
               if (compare(*current, *previous)) break;
               previous = current;
            }
         }
      }
      if (nth < lower) {
         last = lower;
      }
      else {
         first = lower + 1;
      }
   }
}

template <class Compare>
void select_candidate_prefix(std::vector<int>& candidates, int keep, Compare compare) {
   if (keep >= static_cast<int>(candidates.size())) return;
   auto middle = candidates.begin() + keep;
   deterministic_nth_element(candidates.begin(), middle, candidates.end(), compare);
   candidates.resize(static_cast<size_t>(keep));
}

struct IncomingCandidate {
   double key = 0.0;
   int position = -1;
   int variable = -1;
};

struct TabuScratch {
   std::vector<int> tabu_until;
   std::vector<int> ones;
   std::vector<int> zeros;
   std::vector<int> pos_in_ones;
   std::vector<int> pos_in_zeros;
   std::vector<int> cand_out;
   std::vector<int> cand_in;
   std::vector<int> best_x;
   std::vector<int> candidate_position;
   std::vector<uint32_t> candidate_position_generation;
   std::vector<uint32_t> edge_generation;
   std::vector<double> sparse_in_term;
   std::vector<IncomingCandidate> incoming_order;
   uint32_t candidate_marker_generation = 0;
   uint32_t edge_marker_generation = 0;
};

uint32_t next_marker_generation(
   std::vector<uint32_t>& markers,
   uint32_t& generation
) {
   ++generation;
   if (generation == 0) {
      std::fill(markers.begin(), markers.end(), 0U);
      generation = 1;
   }
   return generation;
}

struct MoveChoice {
   int i = -1;
   int j = -1;
   int out_position = -1;
   int in_position = -1;
   double delta = std::numeric_limits<double>::infinity();

   bool valid() const { return i >= 0 && j >= 0; }
};

static inline void consider_move(
   MoveChoice& choice,
   int i,
   int j,
   int out_position,
   int in_position,
   double delta
) {
   if (!choice.valid() || delta < choice.delta ||
      (delta == choice.delta &&
         (out_position < choice.out_position ||
            (out_position == choice.out_position && in_position < choice.in_position)))) {
      choice = { i, j, out_position, in_position, delta };
   }
}

MoveChoice find_dense_tabu_move(
   const Instance& ins,
   const std::vector<int>& cand_out,
   const std::vector<int>& cand_in,
   const std::vector<double>& h,
   const std::vector<int>& tabu_until,
   int iter,
   double obj,
   double best_obj
) {
   const size_t cols = cand_in.size();
   MoveChoice allowed;
   MoveChoice fallback;
   for (size_t a = 0; a < cand_out.size(); ++a) {
      const int i = cand_out[a];
      const double* row = &ins.Q[static_cast<size_t>(i) * static_cast<size_t>(ins.n)];
      for (size_t b = 0; b < cols; ++b) {
         const int j = cand_in[b];
         const double delta =
            2.0 * (h[j] - h[i]) + ins.diagonal[i] + ins.diagonal[j] - 2.0 * row[j];
         consider_move(fallback, i, j, static_cast<int>(a), static_cast<int>(b), delta);
         const bool is_tabu = (iter <= tabu_until[i] || iter <= tabu_until[j]);
         const bool aspiration = (obj + delta < best_obj - 1e-6);
         if (!is_tabu || aspiration) {
            consider_move(allowed, i, j, static_cast<int>(a), static_cast<int>(b), delta);
         }
      }
   }
   return allowed.valid() ? allowed : fallback;
}

MoveChoice find_sparse_tabu_move(
   const Instance& ins,
   const std::vector<int>& cand_out,
   const std::vector<int>& cand_in,
   const std::vector<double>& h,
   const std::vector<int>& tabu_until,
   int iter,
   double obj,
   double best_obj,
   TabuScratch& scratch
) {
   const int cols = static_cast<int>(cand_in.size());
   if (scratch.candidate_position.size() != static_cast<size_t>(ins.n)) {
      scratch.candidate_position.resize(static_cast<size_t>(ins.n));
      scratch.candidate_position_generation.assign(static_cast<size_t>(ins.n), 0U);
   }
   const uint32_t candidate_generation = next_marker_generation(
      scratch.candidate_position_generation,
      scratch.candidate_marker_generation
   );
   for (int b = 0; b < cols; ++b) {
      const int candidate = cand_in[b];
      scratch.candidate_position[candidate] = b;
      scratch.candidate_position_generation[candidate] = candidate_generation;
   }

   scratch.incoming_order.resize(static_cast<size_t>(cols));
   scratch.sparse_in_term.resize(static_cast<size_t>(cols));
   for (int b = 0; b < cols; ++b) {
      const int j = cand_in[b];
      const double incoming_term = 2.0 * h[j] + ins.diagonal[j];
      scratch.sparse_in_term[b] = incoming_term;
      scratch.incoming_order[b] = { incoming_term, b, j };
   }
   std::sort(
      scratch.incoming_order.begin(),
      scratch.incoming_order.end(),
      [](const IncomingCandidate& lhs, const IncomingCandidate& rhs) {
         if (lhs.key != rhs.key) return lhs.key < rhs.key;
         return lhs.position < rhs.position;
      }
   );
   if (scratch.edge_generation.size() < static_cast<size_t>(cols)) {
      scratch.edge_generation.resize(static_cast<size_t>(cols), 0U);
   }

   MoveChoice allowed;
   MoveChoice fallback;
   for (int a = 0; a < static_cast<int>(cand_out.size()); ++a) {
      const int i = cand_out[a];
      const uint32_t generation = next_marker_generation(
         scratch.edge_generation,
         scratch.edge_marker_generation
      );
      const double outgoing_term = -2.0 * h[i] + ins.diagonal[i];

      for (int p = ins.row_ptr[i]; p < ins.row_ptr[i + 1]; ++p) {
         const int j = ins.col_ind[p];
         if (scratch.candidate_position_generation[j] != candidate_generation) continue;
         const int b = scratch.candidate_position[j];
         scratch.edge_generation[b] = generation;
         const double delta = outgoing_term + scratch.sparse_in_term[b] - 2.0 * ins.val[p];
         consider_move(fallback, i, j, a, b, delta);
         const bool is_tabu = (iter <= tabu_until[i] || iter <= tabu_until[j]);
         const bool aspiration = (obj + delta < best_obj - 1e-6);
         if (!is_tabu || aspiration) consider_move(allowed, i, j, a, b, delta);
      }

      bool found_any_non_edge = false;
      bool found_nontabu_non_edge = false;
      for (const IncomingCandidate& incoming : scratch.incoming_order) {
         const int b = incoming.position;
         if (scratch.edge_generation[b] == generation) continue;
         const int j = incoming.variable;
         const double delta = outgoing_term + incoming.key;

         if (!found_any_non_edge) {
            consider_move(fallback, i, j, a, b, delta);
            const bool is_tabu = (iter <= tabu_until[i] || iter <= tabu_until[j]);
            const bool aspiration = (obj + delta < best_obj - 1e-6);
            if (!is_tabu || aspiration) consider_move(allowed, i, j, a, b, delta);
            found_any_non_edge = true;
         }

         if (iter > tabu_until[i] && iter > tabu_until[j]) {
            if (!found_nontabu_non_edge) {
               consider_move(allowed, i, j, a, b, delta);
               found_nontabu_non_edge = true;
            }
         }

         if (found_any_non_edge && (iter <= tabu_until[i] || found_nontabu_non_edge)) break;
      }
   }

   return allowed.valid() ? allowed : fallback;
}

void tabu_search(
   std::vector<int>& x,
   const Instance& ins,
   double& obj,
   std::vector<double>& h,
   TabuScratch& scratch,
   const Params& par,
   const SearchControl& control,
   ObjectiveSense sense,
   PrintScale scale,
   const BestKnownObjective& best_known,
   Result& res
) {
   FCBQP_PROFILE_TIMER(tabu_timer, tabu_seconds);
   const int n = static_cast<int>(x.size());
   const int k = std::accumulate(x.begin(), x.end(), 0);

   if (k != ins.k) {
      throw std::runtime_error("tabu_search: x has wrong cardinality.");
   }

   if (h.size() != static_cast<size_t>(n)) {
      throw std::runtime_error("tabu_search: invalid cached h vector.");
   }
   std::vector<int>& tabu_until = scratch.tabu_until;
   std::vector<int>& ones = scratch.ones;
   std::vector<int>& zeros = scratch.zeros;
   std::vector<int>& pos_in_ones = scratch.pos_in_ones;
   std::vector<int>& pos_in_zeros = scratch.pos_in_zeros;
   tabu_until.assign(static_cast<size_t>(n), -1);
   pos_in_ones.assign(static_cast<size_t>(n), -1);
   pos_in_zeros.assign(static_cast<size_t>(n), -1);
   ones.clear();
   zeros.clear();

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

   std::vector<int>& best_x = scratch.best_x;
   best_x = x;
   double best_obj = obj;

   auto record_target_if_reached = [&](double candidate_internal_obj) {
      if (!best_known.loaded || res.reached_target) {
         return;
      }

      const double output_obj =  output_objective_from_internal(candidate_internal_obj, ins.objective_offset, sense, scale);

      if (target_reached(output_obj, best_known.objective, sense)) {
         res.reached_target = true;
         publish_deterministic_target();

         auto t_now = std::chrono::steady_clock::now();
         res.time_to_target =
            std::chrono::duration<double>(t_now - control.t_start).count();
      }
      };

   record_target_if_reached(best_obj);

   if (res.reached_target || control.should_stop()) {
      return;
   }

   std::vector<int>& cand_out = scratch.cand_out;
   std::vector<int>& cand_in = scratch.cand_in;
   cand_out.clear();
   cand_in.clear();
   cand_out.reserve(n);
   cand_in.reserve(n);
   int no_improve_tabu = 0;

   for (int iter = 0; iter < par.T_max; ++iter) {
      if (control.should_stop()) break;
      account_logical_work();
      FCBQP_PROFILE_INCREMENT(tabu_iterations);
      if (ones.empty() || zeros.empty()) {
         break;
      }

      {
         FCBQP_PROFILE_TIMER(prefix_timer, tabu_prefix_seconds);
         cand_out = ones;
         cand_in = zeros;

         const int num_cand_out = std::min(par.v_1, static_cast<int>(cand_out.size()));
         const int num_cand_in = std::min(par.v_2, static_cast<int>(cand_in.size()));

         select_candidate_prefix(cand_out, num_cand_out, [&](int a, int b) {
            return h[a] > h[b];
            });
         select_candidate_prefix(cand_in, num_cand_in, [&](int a, int b) {
            return h[a] < h[b];
            });
      }

      MoveChoice move;
      {
         FCBQP_PROFILE_TIMER(move_timer, sparse_move_seconds);
         move = ins.sparse
            ? find_sparse_tabu_move(ins, cand_out, cand_in, h, tabu_until, iter, obj, best_obj, scratch)
            : find_dense_tabu_move(ins, cand_out, cand_in, h, tabu_until, iter, obj, best_obj);
      }
      const int best_i = move.i;
      const int best_j = move.j;
      const double best_delta = move.delta;

      if (best_i < 0 || best_j < 0) {
         break;
      }

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

         record_target_if_reached(best_obj);

         if (res.reached_target || control.should_stop()) {
            break;
         }
      }
      else {
         ++no_improve_tabu;
      }

      if (no_improve_tabu >= 2000) {
         break;
      }

      const int T = tabu_tenure_T(no_improve_tabu, par);

      tabu_until[best_i] = iter + T;
      tabu_until[best_j] = iter + (2 * T) / 3;
   }

   x = best_x;
   obj = best_obj;
}

std::vector<int> path_relink_one_direction(
   const Instance& ins,
   const std::vector<int>& start,
   const std::vector<int>& target,
   const Params& par,
   const SearchControl& control,
   DeterministicRng& rng
) {
   FCBQP_PROFILE_TIMER(path_timer, path_seconds);
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
   const std::vector<double>& diagonal = diagonal_values(ins);
   std::vector<int> candidate_position(static_cast<size_t>(n));
   std::vector<uint32_t> candidate_position_generation(static_cast<size_t>(n), 0U);
   std::vector<uint32_t> edge_generation;
   std::vector<IncomingCandidate> incoming_order;
   uint32_t candidate_marker_generation = 0;
   uint32_t edge_marker_generation = 0;
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
   const auto move_less = [](const Move& a, const Move& b) {
      if (a.delta != b.delta) return a.delta < b.delta;
      if (a.i != b.i) return a.i < b.i;
      return a.j < b.j;
      };
   std::vector<int> cand_remove;
   std::vector<int> cand_add;
   cand_remove.reserve(remove_set.size());
   cand_add.reserve(add_set.size());
   std::vector<Move> moves;
   moves.reserve(10);

   while (!remove_set.empty() && !add_set.empty()) {
      if (control.should_stop()) break;
      account_logical_work();
      FCBQP_PROFILE_INCREMENT(path_steps);
      const int A = std::min(par.v_1, static_cast<int>(remove_set.size()));
      const int B = std::min(par.v_2, static_cast<int>(add_set.size()));

      cand_remove = remove_set;
      select_candidate_prefix(cand_remove, A, [&](int a, int b) {
         if (h[a] != h[b]) return h[a] > h[b];
         return a < b;
         });

      cand_add = add_set;
      select_candidate_prefix(cand_add, B, [&](int a, int b) {
         if (h[a] != h[b]) return h[a] < h[b];
         return a < b;
         });

      moves.clear();
      const auto consider_path_move = [&](int i, int j, double delta) {
         const Move candidate{ i, j, delta };
         if (moves.size() < 10) {
            moves.push_back(candidate);
         }
         else {
            auto worst = std::max_element(moves.begin(), moves.end(), move_less);
            if (move_less(candidate, *worst)) *worst = candidate;
         }
         };

      if (ins.sparse) {
         const uint32_t candidate_generation = next_marker_generation(
            candidate_position_generation, candidate_marker_generation
         );
         for (int b = 0; b < static_cast<int>(cand_add.size()); ++b) {
            const int j = cand_add[static_cast<size_t>(b)];
            candidate_position[j] = b;
            candidate_position_generation[j] = candidate_generation;
         }

         incoming_order.resize(cand_add.size());
         for (int b = 0; b < static_cast<int>(cand_add.size()); ++b) {
            const int j = cand_add[static_cast<size_t>(b)];
            incoming_order[static_cast<size_t>(b)] = {
               2.0 * h[j] + diagonal[j], b, j
            };
         }
         std::sort(
            incoming_order.begin(), incoming_order.end(),
            [](const IncomingCandidate& lhs, const IncomingCandidate& rhs) {
               if (lhs.key != rhs.key) return lhs.key < rhs.key;
               return lhs.variable < rhs.variable;
            }
         );
         if (edge_generation.size() < cand_add.size()) {
            edge_generation.resize(cand_add.size(), 0U);
         }

         for (int i : cand_remove) {
            const uint32_t edge_marker = next_marker_generation(
               edge_generation, edge_marker_generation
            );
            const double outgoing_term = -2.0 * h[i] + diagonal[i];
            for (int p = ins.row_ptr[i]; p < ins.row_ptr[i + 1]; ++p) {
               const int j = ins.col_ind[p];
               if (candidate_position_generation[j] != candidate_generation) continue;
               const int b = candidate_position[j];
               edge_generation[static_cast<size_t>(b)] = edge_marker;
               consider_path_move(
                  i, j,
                  outgoing_term + 2.0 * h[j] + diagonal[j] - 2.0 * ins.val[p]
               );
            }

            int non_edges_considered = 0;
            for (const IncomingCandidate& incoming : incoming_order) {
               if (edge_generation[static_cast<size_t>(incoming.position)] == edge_marker) {
                  continue;
               }
               consider_path_move(i, incoming.variable, outgoing_term + incoming.key);
               if (++non_edges_considered == 10) break;
            }
         }
      }
      else {
         for (int i : cand_remove) {
            for (int j : cand_add) {
               const double delta =
                  2.0 * (h[j] - h[i]) + diagonal[i] + diagonal[j] - 2.0 * Qat(ins, i, j);
               consider_path_move(i, j, delta);
            }
         }
      }

      if (moves.empty()) {
         break;
      }

      const int R = std::min<int>(10, static_cast<int>(moves.size()));
      std::sort(moves.begin(), moves.end(), move_less);

      const Move mv = moves[static_cast<size_t>(bounded_random(rng, static_cast<uint64_t>(R)))];

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

std::vector<int> path_relinking(
   const Instance& ins,
   const std::vector<int>& x_current,
   const std::vector<EliteSol>& elite_pool,
   const Params& par,
   const SearchControl& control,
   DeterministicRng& rng
) {
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

   const int d_0 = static_cast<int>(std::ceil(par.d_frac * ins.k));

   double best_elite_obj = elite_pool.front().obj;
   double worst_elite_obj = elite_pool.back().obj;


   const double obj_denom =  std::max(1e-6, worst_elite_obj - best_elite_obj);

   int guide_idx = -1;
   int best_dist = -1;
   double best_score = -std::numeric_limits<double>::infinity();

   for (int e = 0; e < static_cast<int>(elite_pool.size()); ++e) {
      const int d = hamming_distance_to_elite(x_current, elite_pool[e], ins);

      if (d == 0) {
         continue;
      }

      if (d < d_0) {
         continue;
      }

      const double quality_score = (worst_elite_obj - elite_pool[e].obj) / obj_denom;

      const double distance_score = static_cast<double>(d) / (2.0 * std::min(ins.k, ins.n - ins.k));

      const double score = quality_score + distance_score;

      if (score > best_score) {
         best_score = score;
         best_dist = d;
         guide_idx = e;
      }
   }

   if (guide_idx < 0) {
      for (int e = 0; e < static_cast<int>(elite_pool.size()); ++e) {
         const int d = hamming_distance_to_elite(x_current, elite_pool[e], ins);

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

   std::vector<int> best_forward = path_relink_one_direction(ins, x_current, guide, par, control, rng);

   if (control.should_stop()) return best_forward;
   std::vector<int> best_backward = path_relink_one_direction(ins, guide, x_current, par, control, rng);

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

struct SearchPoint {
   std::vector<int> x;
   std::vector<double> h;
   double obj = std::numeric_limits<double>::infinity();
};

SearchPoint make_search_point(std::vector<int> x, const Instance& ins) {
   SearchPoint point;
   point.x = std::move(x);
   point.obj = objective_x(point.x, ins);
   point.h = compute_h_x(point.x, ins);
   return point;
}

SearchPoint random_greedy_construction(
   const Instance& ins,
   const std::vector<int>& C1,
   double random_frac,
   const Params& par,
   const std::vector<EliteSol>& elite_pool,
   const SearchControl& control,
   DeterministicRng& rng
) {
   FCBQP_PROFILE_TIMER(greedy_timer, greedy_seconds);
   FCBQP_PROFILE_INCREMENT(greedy_calls);
   const int n = ins.n;
   const int k = ins.k;
   const std::vector<double>& diagonal = diagonal_values(ins);

   int v_3 = static_cast<int>(std::round(random_frac * k));

   std::vector<int> cand = C1;

   std::sort(cand.begin(), cand.end());
   cand.erase(std::unique(cand.begin(), cand.end()), cand.end());

   v_3 = std::min(v_3, static_cast<int>(cand.size()));

   const int d = static_cast<int>(std::ceil(par.d_frac * ins.k));

   const int max_attempts = elite_pool.empty() ? 1 : par.construction_max_attempts;

   SearchPoint best_point;
   best_point.x.assign(static_cast<size_t>(n), 0);
   int best_min_dist = -1;

   struct AddMove {
      int i;
      double gain;
   };
   const auto add_move_less = [](const AddMove& a, const AddMove& b) {
      return a.gain < b.gain;
      };
   std::vector<AddMove> moves;
   moves.reserve(static_cast<size_t>(n));
   std::vector<int> x(static_cast<size_t>(n));
   std::vector<int> cand_attempt;
   cand_attempt.reserve(cand.size());
   std::vector<int> available;
   available.reserve(static_cast<size_t>(n));
   std::vector<double> h;
   h.reserve(static_cast<size_t>(n));

   for (int attempt = 0; attempt < max_attempts; ++attempt) {
      if (control.should_stop()) return {};
      account_logical_work();
      FCBQP_PROFILE_INCREMENT(greedy_attempts);
      x.assign(static_cast<size_t>(n), 0);
      cand_attempt = cand;
      deterministic_shuffle(cand_attempt.begin(), cand_attempt.end(), rng);
      int ones_now = 0;

      for (int t = 0; t < v_3; ++t) {
         int i = cand_attempt[t];
         if (x[i] == 0) {
            x[i] = 1;
            ++ones_now;
         }
      }
      available.clear();
      for (int i = 0; i < n; ++i) {
         if (!x[i]) available.push_back(i);
      }

      compute_h_x_into(x, ins, h);
      double obj = objective_x(x, ins);

      while (ones_now < k) {
         if (control.should_stop()) {
            for (int i = 0; i < n && ones_now < k; ++i) {
               if (!x[i]) {
                  obj += diagonal[i] + 2.0 * h[i];
                  x[i] = 1;
                  ++ones_now;
                  add_symmetric_column_to_h(h, ins, i, 1.0);
               }
            }
            return { std::move(x), std::move(h), obj };
         }
         account_logical_work();
         FCBQP_PROFILE_INCREMENT(greedy_additions);
         moves.clear();

         for (int i : available) {
            double gain = diagonal[i] + 2.0 * h[i];
            moves.push_back({ i, gain });
         }

         if (moves.empty()) {
            throw std::runtime_error("random_greedy_construction: no free variable left.");
         }

         int R = std::min(20, static_cast<int>(moves.size()));
         if (R < static_cast<int>(moves.size())) {
            auto middle = moves.begin() + R;
            deterministic_nth_element(moves.begin(), middle, moves.end(), add_move_less);
            moves.resize(static_cast<size_t>(R));
         }

         const AddMove selected = moves[static_cast<size_t>(bounded_random(rng, static_cast<uint64_t>(R)))];
         const int best_i = selected.i;

         obj += selected.gain;
         x[best_i] = 1;
         ++ones_now;
         const auto available_it = std::lower_bound(
            available.begin(), available.end(), best_i
         );
         if (available_it == available.end() || *available_it != best_i) {
            throw std::logic_error("Selected construction variable is not available.");
         }
         available.erase(available_it);

         add_symmetric_column_to_h(h, ins, best_i, 1.0);
      }

      if (elite_pool.empty()) {
         return { std::move(x), std::move(h), obj };
      }

      int cur_min_dist = std::numeric_limits<int>::max();
      {
         FCBQP_PROFILE_TIMER(diversity_timer, diversity_seconds);
         const std::vector<uint64_t> bits = packed_solution(x);
         for (const auto& e : elite_pool) {
            cur_min_dist = std::min(cur_min_dist, hamming_distance_packed(bits, e.bits));
         }
      }

      if (cur_min_dist >= d) {
         return { std::move(x), std::move(h), obj };
      }

      if (cur_min_dist > best_min_dist) {
         best_min_dist = cur_min_dist;
         best_point = { std::move(x), std::move(h), obj };
      }
   }

   return best_point;
}

std::vector<int> elite_based_restart(
   const Instance& ins,
   const Params& par,
   const std::vector<EliteSol>& elite_pool,
   const SearchControl& control,
   DeterministicRng& rng
) {
   const int n = ins.n;
   const int k = ins.k;

   if (elite_pool.empty()) {
      std::vector<int> idx(n);
      std::iota(idx.begin(), idx.end(), 0);
      deterministic_shuffle(idx.begin(), idx.end(), rng);

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
      for (int i : e.ones) ++elite_freq[i];
   }

   const int elite_size = static_cast<int>(elite_pool.size());
   const int freq_threshold = static_cast<int>(std::ceil(0.85 * elite_size));

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

   const int max_fixed = (k > 1) ? std::max(1, static_cast<int>(std::floor(0.85 * k))) : 0;
   if (static_cast<int>(fixed_ones.size()) > max_fixed) {
      fixed_ones.resize(max_fixed);
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

   const int d = static_cast<int>(std::ceil(par.d_frac * k));

   const int max_attempts = 200;

   std::vector<int> best_x(n, 0);
   int best_min_dist = -1;
   std::vector<int> x(static_cast<size_t>(n));
   std::vector<int> cand;
   cand.reserve(free_vars.size());

   for (int attempt = 0; attempt < max_attempts; ++attempt) {
      if (control.should_stop()) return {};
      account_logical_work();
      x.assign(static_cast<size_t>(n), 0);
      for (int i : fixed_ones) {
         x[i] = 1;
      }

      cand = free_vars;
      deterministic_shuffle(cand.begin(), cand.end(), rng);

      for (int t = 0; t < need; ++t) {
         x[cand[t]] = 1;
      }

      int cur_min_dist = std::numeric_limits<int>::max();
      const std::vector<uint64_t> bits = packed_solution(x);
      for (const auto& e : elite_pool) {
         cur_min_dist = std::min(cur_min_dist, hamming_distance_packed(bits, e.bits));
      }

      if (cur_min_dist >= d) {
         return x;
      }

      if (cur_min_dist > best_min_dist) {
         best_min_dist = cur_min_dist;
         best_x = std::move(x);
      }
   }

   return best_x;
}
std::vector<int> random_diverse_restart(
   const Instance& ins,
   const Params& par,
   DeterministicRng& rng,
   const std::vector<EliteSol>& elite_pool,
   const SearchControl& control
) {
   const int n = ins.n;
   const int k = ins.k;

   std::vector<int> idx(n);
   std::iota(idx.begin(), idx.end(), 0);

   const int d = static_cast<int>(std::ceil(par.d_frac * ins.k));

   const int max_attempts = 200;

   std::vector<int> best_x(n, 0);
   int best_min_dist = -1;
   std::vector<int> x(static_cast<size_t>(n));

   for (int attempt = 0; attempt < max_attempts; ++attempt) {
      if (control.should_stop()) return {};
      account_logical_work();
      deterministic_shuffle(idx.begin(), idx.end(), rng);
      x.assign(static_cast<size_t>(n), 0);
      for (int t = 0; t < k; ++t) {
         x[idx[t]] = 1;
      }

      if (elite_pool.empty()) {
         return x;
      }

      int cur_min_dist = std::numeric_limits<int>::max();
      const std::vector<uint64_t> bits = packed_solution(x);
      for (const auto& e : elite_pool) {
         cur_min_dist = std::min(cur_min_dist, hamming_distance_packed(bits, e.bits));
      }

      if (cur_min_dist >= d) {
         return x;
      }

      if (cur_min_dist > best_min_dist) {
         best_min_dist = cur_min_dist;
         best_x = std::move(x);
      }
   }

   return best_x;
}

struct WorkerSearchState {
   explicit WorkerSearchState(uint64_t seed) : rng(seed) {}

   DeterministicRng rng;
   Result result;
   Result epoch_baseline_result;
   uint64_t epoch_work = 0;
   bool epoch_participated = false;
   int no_improve = 0;
   int starts_completed = 0;
   bool finished = false;
   std::vector<int> incumbent_for_guide;
   std::vector<int> candidate_x;
   TabuScratch tabu_scratch;
#if defined(FCBQP_ENABLE_PHASE_PROFILING)
   PhaseProfile phase_profile;
#endif
   double candidate_obj = std::numeric_limits<double>::infinity();
   bool candidate_ready = false;
};

void run_worker_start(
   const Instance& ins,
   const Params& par,
   WorkerSearchState& state,
   int lane_id,
   const EliteState& elite_state,
   DeterministicTargetControl& target_control,
   const SearchControl& control,
   ObjectiveSense sense,
   PrintScale scale,
   const BestKnownObjective& best_known
) {
   apply_schedule_test_delay(lane_id);
   state.candidate_ready = false;
   state.epoch_participated = true;
   state.epoch_work = 0;
   state.epoch_baseline_result = state.result;
   LogicalWorkContext logical_context{
      &target_control,
      &state.epoch_work
   };
   LogicalWorkScope logical_scope(logical_context);
#if defined(FCBQP_ENABLE_PHASE_PROFILING)
   PhaseProfileScope phase_profile_scope(state.phase_profile);
#endif
   if (state.finished || state.result.reached_target || control.should_stop()) return;

   account_logical_work();
   const int start = state.starts_completed++;
   const std::vector<EliteSol>& elite_snapshot = elite_state.pool;

   SearchPoint point;
   if (!elite_snapshot.empty() && state.no_improve >= 10) {
      point = make_search_point(
         path_relinking(ins, state.incumbent_for_guide, elite_snapshot, par, control, state.rng), ins
      );
      state.no_improve = 0;
   }
   else {
      if (!state.incumbent_for_guide.empty() && (start % 10 == 0)) {
         point = make_search_point(elite_based_restart(ins, par, elite_snapshot, control, state.rng), ins);
      }
      else if (start % 10 == 9) {
         point = make_search_point(random_diverse_restart(ins, par, state.rng, elite_snapshot, control), ins);
      }
      else {
         point = random_greedy_construction(
            ins,
            elite_state.construction_candidates,
            par.balance,
            par,
            elite_snapshot,
            control,
            state.rng
         );
      }
   }

   if (point.x.empty()) {
      state.finished = true;
      return;
   }

   tabu_search(
      point.x,
      ins,
      point.obj,
      point.h,
      state.tabu_scratch,
      par,
      control,
      sense,
      scale,
      best_known,
      state.result
   );

   const bool stop_after_tabu = state.result.reached_target || control.should_stop();
   if (!stop_after_tabu) {
      state.candidate_x = point.x;
      state.candidate_obj = point.obj;
      state.candidate_ready = true;
   }

   if (point.obj < state.result.best_obj) {
      state.result.best_obj = point.obj;
      state.result.best_x = point.x;
      state.result.start_to_best = state.starts_completed;

      const auto t_now = std::chrono::steady_clock::now();
      state.result.time_to_best = std::chrono::duration<double>(t_now - control.t_start).count();

      state.incumbent_for_guide = point.x;
      state.no_improve = 0;

      const double output_obj = output_objective_from_internal(state.result.best_obj, ins.objective_offset, sense, scale);
      if (best_known.loaded && target_reached(output_obj, best_known.objective, sense)) {
         if (!state.result.reached_target) {
            state.result.reached_target = true;
            publish_deterministic_target();
            const auto t_target_now = std::chrono::steady_clock::now();
            state.result.time_to_target =
               std::chrono::duration<double>(t_target_now - control.t_start).count();
         }
      }
   }
   else {
      ++state.no_improve;
   }

   if (stop_after_tabu) state.finished = true;
}

std::optional<Result> solve_trivial_cardinality(const Instance& ins) {
   if (ins.k > 1 && ins.k < ins.n) return std::nullopt;

   Result result;
   result.best_x.assign(static_cast<size_t>(ins.n), 0);
   if (ins.k == ins.n) {
      std::fill(result.best_x.begin(), result.best_x.end(), 1);
   }
   else if (ins.k == 1) {
      const std::vector<double>& diagonal = diagonal_values(ins);
      const auto best = std::min_element(diagonal.begin(), diagonal.end());
      if (best != diagonal.end()) result.best_x[static_cast<size_t>(best - diagonal.begin())] = 1;
   }
   result.best_obj = objective_x(result.best_x, ins);
   result.time_to_best = 0.0;
   return result;
}

int main(int argc, char* argv[]) {
   try {
      if (argc < 4) {
         std::cerr << "Usage: " << argv[0]
            << " <max|min> <full|half> <instance.qsc> [best_known_results.csv_or_folder]"
            << " [base_seed] [runs] [time_limit_seconds] [index_base] [workers] [starts]\n";
         return 1;
      }

      const ObjectiveSense sense = parse_objective_sense(argv[1]);
      const PrintScale scale = parse_print_scale(argv[2]);
      const std::string instance_path = argv[3];
      const std::string best_known_arg = (argc >= 5 ? std::string(argv[4]) : std::string());
      const uint64_t base_seed = (argc >= 6 ? std::stoull(argv[5]) : 1ULL);
      const int num_runs = (argc >= 7 ? std::stoi(argv[6]) : 10);
      const double requested_time_limit = (argc >= 8 ? std::stod(argv[7]) : 600.0);
      std::optional<int> forced_index_base;
      if (argc >= 9) {
         const std::string base_arg = argv[8];
         if (base_arg == "0" || base_arg == "1") forced_index_base = std::stoi(base_arg);
         else if (base_arg != "auto") throw std::runtime_error("Index base must be 'auto', 0, or 1.");
      }
      const int requested_workers = (argc >= 10)
         ? parse_worker_count(argv[9], "Worker count")
         : default_worker_count();
      const int requested_starts = (argc >= 11)
         ? parse_worker_count(argv[10], "Start count")
         : 200000;

      if (num_runs <= 0) {
         throw std::runtime_error("Number of runs must be positive.");
      }
      if (requested_time_limit < 0.0) {
         throw std::runtime_error("Time limit must be nonnegative (zero disables it).");
      }

      Instance ins = read_instance(instance_path, sense, forced_index_base);
      const int original_k = ins.k;
      const bool canonical_inverted_from_input = canonicalize_instance(ins);
      const std::string best_known_path = resolve_best_known_objective_path(instance_path, best_known_arg);
      BestKnownObjective best_known;
      if (!best_known_path.empty()) {
         best_known = read_best_known_objective_file(best_known_path, instance_path);
      }

      Params par;
      par.num_starts = requested_starts;
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

      par.time_limit_seconds = requested_time_limit;
      if (const char* configured = std::getenv("FCBQP_EPOCH_LANES_PER_WORKER")) {
         par.epoch_lanes_per_worker = parse_worker_count(
            configured, "FCBQP_EPOCH_LANES_PER_WORKER"
         );
      }
      if (const char* configured = std::getenv("FCBQP_CONSTRUCTION_MAX_ATTEMPTS")) {
         par.construction_max_attempts = parse_worker_count(
            configured, "FCBQP_CONSTRUCTION_MAX_ATTEMPTS"
         );
      }
      if (const char* configured = std::getenv("FCBQP_ELITE_GROUP_SIZE")) {
         par.elite_group_size = parse_worker_count(
            configured, "FCBQP_ELITE_GROUP_SIZE"
         );
      }
      const int configured_epoch_lanes = static_cast<int>(std::min<int64_t>(
         requested_starts,
         static_cast<int64_t>(requested_workers) * par.epoch_lanes_per_worker
      ));
      const int configured_elite_group_size = std::min(
         par.elite_group_size, configured_epoch_lanes
      );

      const std::optional<Result> trivial_result = solve_trivial_cardinality(ins);
      std::vector<int> C1;
      if (!trivial_result.has_value()) {
         const std::vector<double> impact = interaction_impact(ins, requested_workers);
         C1 = build_set(ins, par, impact);
      }

      std::vector<double> run_output_objs;
      run_output_objs.reserve(num_runs);
      std::vector<double> reached_tts;
      reached_tts.reserve(num_runs);

      double best_output_obj = (sense == ObjectiveSense::Max)
         ? -std::numeric_limits<double>::infinity()
         : std::numeric_limits<double>::infinity();
      Result best_run_res;
      best_run_res.best_obj = std::numeric_limits<double>::infinity();
      std::cout << "Sense: " << objective_sense_name(sense) << "\n";
      std::cout << "Scale: " << print_scale_name(scale) << "\n";
      std::cout << "Runs: " << num_runs << "\n";
      std::cout << "Workers: " << requested_workers << "\n";
      std::cout << "Epoch lanes: " << configured_epoch_lanes << "\n";
      std::cout << "Elite group size: " << configured_elite_group_size << "\n";
      std::cout << "Starts: " << requested_starts << "\n";
      std::cout << "Construction attempts: " << par.construction_max_attempts << "\n";
      std::cout << "Parallel backend: " << parallel_backend_name() << "\n";

      if (best_known.loaded) {
         std::cout << "Best-known objective = " << std::fixed << std::setprecision(9) << best_known.objective << "\n";
      }
      else {
         std::cout << "Best-known objective file = not found or no matching instance\n";
      }

      for (int run = 1; run <= num_runs; ++run) {
         auto t0 = std::chrono::steady_clock::now();

         Result global_res;
         global_res.best_obj = std::numeric_limits<double>::infinity();
         uint64_t deterministic_logical_work = 0;
#if defined(FCBQP_ENABLE_PHASE_PROFILING)
         PhaseProfile run_profile;
#endif
         bool run_reached_target = false;
         double run_time_to_target = std::numeric_limits<double>::infinity();
         if (trivial_result.has_value()) {
            global_res = *trivial_result;
            const double trivial_output = output_objective_from_internal(global_res.best_obj, ins.objective_offset, sense, scale);
            if (best_known.loaded && target_reached(trivial_output, best_known.objective, sense)) {
               run_reached_target = true;
               run_time_to_target = 0.0;
            }
         }
         else {
            std::atomic<bool> stop_requested(false);
            SearchControl control{ t0, par.time_limit_seconds, stop_requested };

            const int worker_count = std::min(requested_workers, par.num_starts);
            const int lane_count = std::min(configured_epoch_lanes, par.num_starts);
            const int elite_group_size = std::min(configured_elite_group_size, lane_count);
            const int elite_group_count =
               (lane_count + elite_group_size - 1) / elite_group_size;
            std::vector<EliteState> elite_groups(static_cast<size_t>(elite_group_count));
            for (EliteState& elite : elite_groups) {
               elite.ones_count.assign(static_cast<size_t>(ins.n), 0);
               elite.construction_candidates = C1;
            }
            std::vector<int> lane_start_limits(static_cast<size_t>(lane_count));
            std::vector<WorkerSearchState> lane_states;
            lane_states.reserve(static_cast<size_t>(lane_count));
            std::vector<std::exception_ptr> lane_errors(static_cast<size_t>(lane_count));

            const int base = par.num_starts / lane_count;
            const int extra = par.num_starts % lane_count;
            for (int lane_id = 0; lane_id < lane_count; ++lane_id) {
               lane_start_limits[lane_id] = base + (lane_id < extra ? 1 : 0);
               const uint64_t run_seed = base_seed
                  + 0xD1B54A32D192ED03ULL * static_cast<uint64_t>(run)
                  + 0x9E3779B97F4A7C15ULL * static_cast<uint64_t>(lane_id + 1);
               lane_states.emplace_back(run_seed);
            }

            int search_epoch = 0;
            while (true) {
               bool have_active_lane = false;
               for (int lane_id = 0; lane_id < lane_count; ++lane_id) {
                  const WorkerSearchState& state = lane_states[lane_id];
                  if (!state.finished && !state.result.reached_target &&
                     state.starts_completed < lane_start_limits[lane_id]) {
                     have_active_lane = true;
                     break;
                  }
               }
               if (!have_active_lane || control.should_stop()) break;

               for (WorkerSearchState& state : lane_states) {
                  state.epoch_participated = false;
                  state.epoch_work = 0;
               }
               DeterministicTargetControl epoch_target;
               std::atomic<int> next_lane{ 0 };
#if defined(FCBQP_ENABLE_PHASE_PROFILING)
               const auto parallel_begin = std::chrono::steady_clock::now();
#endif
               parallel_for_workers(worker_count, [&](int) noexcept {
                  while (true) {
                     const int lane_id = next_lane.fetch_add(1, std::memory_order_relaxed);
                     if (lane_id >= lane_count) break;
                     WorkerSearchState& state = lane_states[lane_id];
                     if (state.finished || state.result.reached_target ||
                        state.starts_completed >= lane_start_limits[lane_id]) {
                        continue;
                     }
                     try {
                        run_worker_start(
                           ins,
                           par,
                           state,
                           lane_id,
                           elite_groups[static_cast<size_t>(lane_id / elite_group_size)],
                           epoch_target,
                           control,
                           sense,
                           scale,
                           best_known
                        );
                     }
                     catch (...) {
                        lane_errors[lane_id] = std::current_exception();
                        stop_requested.store(true, std::memory_order_relaxed);
                     }
                  }
                  });
#if defined(FCBQP_ENABLE_PHASE_PROFILING)
               run_profile.parallel_seconds += std::chrono::duration<double>(
                  std::chrono::steady_clock::now() - parallel_begin
               ).count();
#endif

               for (const std::exception_ptr& error : lane_errors) {
                  if (error) std::rethrow_exception(error);
               }

               const uint64_t target_cutoff = epoch_target.logical_cutoff.load(
                  std::memory_order_acquire
               );
               for (const WorkerSearchState& state : lane_states) {
                  if (!state.epoch_participated) continue;
                  deterministic_logical_work +=
                     (target_cutoff == std::numeric_limits<uint64_t>::max())
                     ? state.epoch_work
                     : std::min(state.epoch_work, target_cutoff);
               }
               if (target_cutoff != std::numeric_limits<uint64_t>::max()) {
                  for (WorkerSearchState& state : lane_states) {
                     if (state.epoch_work > target_cutoff) {
                        state.result = state.epoch_baseline_result;
                        state.candidate_ready = false;
                        state.candidate_x.clear();
                     }
                  }
               }

               ++search_epoch;
               bool epoch_reached_target = false;
               for (const WorkerSearchState& state : lane_states) {
                  epoch_reached_target = epoch_reached_target || state.result.reached_target;
               }
               if (epoch_reached_target) {
                  global_res.target_epoch = search_epoch;
                  break;
               }

#if defined(FCBQP_ENABLE_PHASE_PROFILING)
               const auto elite_merge_begin = std::chrono::steady_clock::now();
#endif
               std::vector<unsigned char> elite_changed(
                  static_cast<size_t>(elite_group_count), 0
               );
               for (int lane_id = 0; lane_id < lane_count; ++lane_id) {
                  WorkerSearchState& state = lane_states[lane_id];
                  if (state.candidate_ready) {
                     const int group = lane_id / elite_group_size;
                     EliteState& elite = elite_groups[static_cast<size_t>(group)];
                     elite_changed[static_cast<size_t>(group)] = try_insert_elite(
                        ins,
                        elite.pool,
                        elite.ones_count,
                        state.candidate_x,
                        state.candidate_obj,
                        par
                     ) || elite_changed[static_cast<size_t>(group)];
                     state.candidate_ready = false;
                     state.candidate_x.clear();
                  }
               }
               for (int group = 0; group < elite_group_count; ++group) {
                  if (!elite_changed[static_cast<size_t>(group)]) continue;
                  EliteState& elite = elite_groups[static_cast<size_t>(group)];
                  elite.construction_candidates = C1;
                  for (int i = 0; i < ins.n; ++i) {
                     if (elite.ones_count[i] > 0) {
                        elite.construction_candidates.push_back(i);
                     }
                  }
                  std::sort(
                     elite.construction_candidates.begin(),
                     elite.construction_candidates.end()
                  );
                  elite.construction_candidates.erase(
                     std::unique(
                        elite.construction_candidates.begin(),
                        elite.construction_candidates.end()
                     ),
                     elite.construction_candidates.end()
                  );
               }
#if defined(FCBQP_ENABLE_PHASE_PROFILING)
               run_profile.elite_merge_seconds += std::chrono::duration<double>(
                  std::chrono::steady_clock::now() - elite_merge_begin
               ).count();
#endif

            }

            for (WorkerSearchState& state : lane_states) {
#if defined(FCBQP_ENABLE_PHASE_PROFILING)
               run_profile += state.phase_profile;
#endif
               Result& local_res = state.result;
               const bool local_reached_target = local_res.reached_target && std::isfinite(local_res.time_to_target);
               if (local_reached_target && (!run_reached_target || local_res.time_to_target < run_time_to_target)) {
                  run_reached_target = true;
                  run_time_to_target = local_res.time_to_target;
               }
               const bool better_objective = local_res.best_obj < global_res.best_obj;
               const bool canonical_tie = local_res.best_obj == global_res.best_obj &&
                  !local_res.best_x.empty() &&
                  (global_res.best_x.empty() || local_res.best_x < global_res.best_x);
               if (better_objective || canonical_tie) {
                  const int target_epoch = global_res.target_epoch;
                  global_res = std::move(local_res);
                  global_res.target_epoch = target_epoch;
               }
            }
         }
         if (run_reached_target) {
            global_res.reached_target = true;
            global_res.time_to_target = run_time_to_target;
         }
         else {
            global_res.reached_target = false;
            global_res.time_to_target = std::numeric_limits<double>::quiet_NaN();
         }
         auto t1 = std::chrono::steady_clock::now();
         const double elapsed = std::chrono::duration<double>(t1 - t0).count();
         const double output_obj = output_objective_from_internal(global_res.best_obj, ins.objective_offset, sense, scale);
         const bool reached_best_known = best_known.loaded && target_reached(output_obj, best_known.objective, sense);

         run_output_objs.push_back(output_obj);
         if (global_res.reached_target && std::isfinite(global_res.time_to_target)) {
            reached_tts.push_back(global_res.time_to_target);
         }

         const bool improves_best = (sense == ObjectiveSense::Max)
            ? (output_obj > best_output_obj + 1e-6)
            : (output_obj < best_output_obj - 1e-6);
         if (improves_best) {
            best_output_obj = output_obj;
            best_run_res = global_res;
         }

         std::cout << "Run " << run << "/" << num_runs
            << ": best objective = " << std::fixed << std::setprecision(9) << output_obj
            << ", reached best-known = " << (reached_best_known ? "YES" : "NO");

         if (global_res.reached_target) {
            std::cout << ", TTS to best-known (s) = "
               << std::fixed << std::setprecision(6) << global_res.time_to_target
               << ", target epoch = " << global_res.target_epoch;
         }
         else {
            std::cout << ", TTS to best-known (s) = unknown, target epoch = unknown";
         }

         std::cout << ", start to run-best = " << global_res.start_to_best
            << ", time to run-best (s) = "
            << std::fixed << std::setprecision(6) << global_res.time_to_best
            << ", runtime (s) = "
            << std::fixed << std::setprecision(6) << elapsed
            << "\n";
         std::cout << "Logical work: deterministic frontier = "
            << deterministic_logical_work << "\n";
#if defined(FCBQP_ENABLE_PHASE_PROFILING)
         std::cout << "Profile: parallel_s=" << run_profile.parallel_seconds
            << ", greedy_cpu_s=" << run_profile.greedy_seconds
            << ", greedy_calls=" << run_profile.greedy_calls
            << ", greedy_attempts=" << run_profile.greedy_attempts
            << ", greedy_additions=" << run_profile.greedy_additions
            << ", diversity_cpu_s=" << run_profile.diversity_seconds
            << ", tabu_cpu_s=" << run_profile.tabu_seconds
            << ", tabu_iterations=" << run_profile.tabu_iterations
            << ", tabu_prefix_cpu_s=" << run_profile.tabu_prefix_seconds
            << ", sparse_move_cpu_s=" << run_profile.sparse_move_seconds
            << ", path_cpu_s=" << run_profile.path_seconds
            << ", path_steps=" << run_profile.path_steps
            << ", elite_merge_s=" << run_profile.elite_merge_seconds
            << "\n";
#endif
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

      const std::string instance_name = basename_without_extension(instance_path);

      std::vector<int> reported_best_x = best_run_res.best_x;
      if (canonical_inverted_from_input) {
         for (int& value : reported_best_x) value = 1 - value;
      }
      if (!reported_best_x.empty() &&
         std::accumulate(reported_best_x.begin(), reported_best_x.end(), 0) != original_k) {
         throw std::logic_error("Reported solution has the wrong original cardinality.");
      }

      std::cout << "\nSummary over " << num_runs << " runs\n";
      std::cout << "Instance name = " << instance_name << "\n";
      std::cout << "Best-known obj from CSV = " << (best_known.loaded ? best_known_str : "") << "\n";
      std::cout << "Best found obj = " << std::fixed << std::setprecision(9) << best_output_obj << "\n";
      std::ostringstream fingerprint;
      fingerprint << std::hex << std::setw(16) << std::setfill('0')
         << solution_fingerprint(reported_best_x);
      std::cout << "Solution fingerprint = " << fingerprint.str() << "\n";
      std::cout << "Runtime limit: " << static_cast<int>(par.time_limit_seconds) << " seconds\n";
      std::cout << "Average obj = " << std::fixed << std::setprecision(9) << average_obj << "\n";
      std::cout << "Average TTS = " << average_tts_str << "\n";
      std::cout << "Times reaching best-known = " << reached_count << "\n";

      if (!best_run_res.best_x.empty()) {
         double recomputed_obj = objective_x(best_run_res.best_x, ins);
         double diff = recomputed_obj - best_run_res.best_obj;
         if (std::abs(diff) > 1e-5) {
            std::cout << "Wrong obj value !!\n";
         }

         std::cout << "Selected vars of best run: ";
         bool first_selected = true;
         for (int i = 0; i < ins.n; i++) {
            if (reported_best_x[i] == 1) {
               if (!first_selected) std::cout << " ";
               std::cout << (i + 1);
               first_selected = false;
            }
         }
         std::cout << "\n";
      }

      return 0;
   }
   catch (const std::exception& e) {
      std::cerr << "ERROR: " << e.what() << "\n";
      return 1;
   }
}
