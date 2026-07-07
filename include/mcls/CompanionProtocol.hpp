#pragma once

#include "mcls/ServiceSnapshot.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mcls {

/// Parsed inbound companion request.
struct CompanionRequest {
    int version = 0;
    int id = 0;       ///< echo'd back in response
    std::string op;   ///< "status", "fc.logs", "caps", "archive.start", "archive.cancel"
    std::string token;
    std::string client;  ///< optional GS identifier; echoed verbatim in the response when present

    // fc.logs pagination
    int offset = 0;
    int limit = 8;

    // logs.download selection (Phase B)
    std::vector<std::uint16_t> sel_ids;
    bool sel_all = false;
};

/// Result of serializing a response — carries the final JSON string.
struct SerializedResponse {
    std::string json;
    bool truncated = false;  ///< true if content was reduced to fit budget
};

/// JSON serialization / deserialization for the companion protocol.
///
/// All serialize functions enforce max_bytes via serializeWithBudget().
/// Returns empty optional on parse failure (caller should send bad_request).
namespace CompanionProtocol {

/// Upper bound on explicit ids per logs.download request (advertised in caps).
/// Larger selections should use "sel":{"all":true}.
inline constexpr int kMaxIdsPerRequest = 32;

std::optional<CompanionRequest> parseRequest(const std::string& json_text);

/// All builders take the request's optional `client` string and echo it
/// verbatim in the response when non-empty (multi-client response filtering).

/// Build a Tier 1 status response.
SerializedResponse buildStatus(int request_id,
                               const ServiceSnapshot& snap,
                               std::size_t max_bytes,
                               const std::string& client = {});

/// Build a Tier 2 fc.logs paginated response.
SerializedResponse buildFcLogs(int request_id,
                               const FcLogsPage& page,
                               std::size_t max_bytes,
                               const std::string& client = {});

/// Build a simple ack response for mutating commands.
SerializedResponse buildAck(int request_id,
                            bool ok,
                            const std::string& err = {},
                            const std::string& client = {});

/// Build a job ack (archive.start / logs.refresh / logs.erase). Always ok=true:
/// a request that lands while the job is already running is idempotent success
/// (already_running=true), not an error, so a client's retry after a lost ack
/// reads as success.
SerializedResponse buildJobAck(int request_id,
                               bool already_running,
                               const std::string& client = {});

/// Build the logs.download ack: how many logs were queued and which requested
/// ids were not present in the current enumeration.
SerializedResponse buildDownloadAck(int request_id,
                                    int queued,
                                    const std::vector<std::uint16_t>& not_found,
                                    const std::string& client = {});

/// Build a caps response advertising protocol version, supported ops, and limits.
SerializedResponse buildCaps(int request_id,
                             int max_request_bytes,
                             int max_response_bytes,
                             int max_fc_logs_per_response,
                             std::size_t max_bytes,
                             const std::string& client = {});

/// Build an error response.
SerializedResponse buildError(int request_id,
                              const std::string& err,
                              const std::string& client = {});

} // namespace CompanionProtocol
} // namespace mcls
