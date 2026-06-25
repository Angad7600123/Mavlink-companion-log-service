#include "mcls/Sha256.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

TEST(Sha256Test, KnownVectorAndCloneProbe) {
    const std::string payload(60000, 'a');
    mcls::Sha256 hasher;
    const std::size_t probe_n = 51200;

    hasher.update(reinterpret_cast<const uint8_t*>(payload.data()), probe_n);
    const std::string probe_hash = hasher.clone().finalizeHex();

    hasher.update(reinterpret_cast<const uint8_t*>(payload.data() + probe_n),
                  payload.size() - probe_n);
    const std::string full_hash = hasher.finalizeHex();

    const std::string expected_full =
        mcls::Sha256::hashHex(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
    const std::string expected_probe = mcls::Sha256::hashHex(
        reinterpret_cast<const uint8_t*>(payload.data()), probe_n);

    EXPECT_EQ(full_hash, expected_full);
    EXPECT_EQ(probe_hash, expected_probe);
    EXPECT_NE(full_hash, probe_hash);
}
