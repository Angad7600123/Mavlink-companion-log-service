#pragma once

#include "mcls/ServiceSnapshot.hpp"

#include <cstddef>
#include <optional>
#include <string>

namespace mcls {

/// Parsed inbound companion request.
struct CompanionRequest {
    int version = 0;
    int id = 0;       ///< echo'd back in response
    std::string op;   ///< "status", "fc.logs", "archive.start", "archive.cancel"
    std::string token;

    // fc.logs pagination
    int offset = 0;
    int limit = 8;
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

std::optional<CompanionRequest> parseRequest(const std::string& json_text);

/// Build a Tier 1 status response.
SerializedResponse buildStatus(int request_id,
                               const ServiceSnapshot& snap,
                               std::size_t max_bytes);

/// Build a Tier 2 fc.logs paginated response.
SerializedResponse buildFcLogs(int request_id,
                               const FcLogsPage& page,
                               std::size_t max_bytes);

/// Build a simple ack response for mutating commands.
SerializedResponse buildAck(int request_id, bool ok, const std::string& err = {});

/// Build an error response.
SerializedResponse buildError(int request_id, const std::string& err);

} // namespace CompanionProtocol
} // namespace mcls
