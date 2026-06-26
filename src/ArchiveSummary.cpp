#include "mcls/ArchiveSummary.hpp"

#include <sstream>
#include <iomanip>

namespace mcls {

namespace {

const char* outcomeStr(VerificationOutcome o) {
    switch (o) {
    case VerificationOutcome::Ok: return "ok";
    case VerificationOutcome::Failed: return "failed";
    case VerificationOutcome::Skipped: return "skipped";
    }
    return "unknown";
}

} // namespace

std::string ArchivePerformanceSummary::formatLine() const {
    std::ostringstream os;
    os << "ArchiveSummary log_id=" << log_id << " size=" << total_size << " duration_ms=" << duration_ms
       << " avg_kbps=" << std::fixed << std::setprecision(1) << avg_throughput_kbps << " duplicates="
       << download.duplicate_packets << " gap_fills=" << download.gap_fill_requests
       << " overlap_conflicts=" << download.overlap_conflicts << " parse=" << outcomeStr(parse_result)
       << " sample_reread=" << outcomeStr(sample_reread_result)
       << " sha256=" << (sha256.empty() ? "-" : sha256) << " decision=" << final_decision
       << " erase=" << log_erase;
    return os.str();
}

} // namespace mcls
