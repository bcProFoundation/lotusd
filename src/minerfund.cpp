// Copyright (c) 2020 The Bitcoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <minerfund.h>

#include <cashaddrenc.h> // For DecodeCashAddrContent
#include <chain.h>       // For CBlockIndex class
#include <chainparams.h>
#include <consensus/activation.h>
#include <key_io.h> // For DecodeDestination

#include <cstdint>

static const CTxOut BuildOutput(const std::string &address,
                                const Amount amount) {
    const std::unique_ptr<CChainParams> mainNetParams =
        CreateChainParams(CBaseChainParams::MAIN);
    CTxDestination dest = DecodeDestination(address, *mainNetParams);
    if (!IsValidDestination(dest)) {
        // Try the ecash cashaddr prefix.
        dest = DecodeCashAddrDestination(
            DecodeCashAddrContent(address, "bitcoincash"));
    }
    const CScript script = GetScriptForDestination(dest);
    return {amount, script};
}
static const CTxOut BuildBurnOutput(const Amount amount) {
    // Create a script with OP_RETURN to burn the amount
    CScript scriptPubKey;
    scriptPubKey << OP_RETURN;
    return {amount, scriptPubKey};
}

// Build output with capped amount. The remaining is burned.
static const std::vector<CTxOut>
BuildOutputsCyclingCapped(const std::vector<std::string> &addresses,
                          const CBlockIndex *pindexPrev, 
                          const Amount blockReward, 
                          const Consensus::Params& params, 
                          size_t bodhiIndex) {
    std::vector<CTxOut> outputs;
    const size_t numAddresses = addresses.size();
    const auto blockHeight = pindexPrev->nHeight + 1;
    const auto addressIndx = blockHeight % numAddresses;

    // Share amount is now divided into 3 buckets, each capped at 1/3 of the current cap limit
    Amount fundingAmount = std::min(blockReward / 2, params.bodhiCappedFundingAmount);
    Amount bucketAmount = fundingAmount / 3;

    // The address to pay out to based on the block height
    const auto address = addresses[addressIndx];

    // Add output for the selected address
    outputs.push_back(BuildOutput(address, bucketAmount));

    // Handle Staking Rewards
    if (params.bodhiStakingRewardsActivation >= bodhiIndex) {
        outputs.push_back(BuildStakingRewardsOutput(bucketAmount));
    } else {
        outputs.push_back(BuildBurnOutput(bucketAmount));
    }

    // Handle Community Fund
    if (params.bodhiCommunityFundActivation >= bodhiIndex) {
        outputs.push_back(BuildCommunityFundOutput(bucketAmount));
    } else {
        outputs.push_back(BuildBurnOutput(bucketAmount));
    }

    // Calculate the remaining amount after distribution to the selected address
    Amount remaining = fundingAmount - (bucketAmount * 3);

    // If there's anything left after paying out to the address, staking rewards, and community fund, send it to the burn address
    if (remaining > 0) {
        outputs.push_back(BuildBurnOutput(remaining));
    }

    return outputs;
}

static const std::vector<CTxOut>
BuildOutputsCycling(const std::vector<std::string> &addresses,
                    const CBlockIndex *pindexPrev, const Amount blockReward) {
    const size_t numAddresses = addresses.size();
    const Amount shareAmount = blockReward / 2;
    const auto blockHeight = pindexPrev->nHeight + 1;
    const auto addressIndx = blockHeight % numAddresses;
    const auto address = addresses[addressIndx];
    return std::vector<CTxOut>({BuildOutput(address, shareAmount)});
}

static const std::vector<CTxOut>
BuildOutputsFanOut(const std::vector<std::string> &addresses,
                   const CBlockIndex *pindexPrev, const Amount blockReward) {
    const size_t numAddresses = addresses.size();
    std::vector<CTxOut> minerFundOutputs;
    const Amount shareAmount = blockReward / (int64_t(2 * numAddresses));
    minerFundOutputs.reserve(numAddresses);
    for (const std::string &address : addresses) {
        minerFundOutputs.emplace_back(BuildOutput(address, shareAmount));
    }
    return minerFundOutputs;
}

std::vector<CTxOut> GetMinerFundRequiredOutputs(const Consensus::Params &params,
                                                const bool enableMinerFund,
                                                const CBlockIndex *pindexPrev,
                                                const Amount &blockReward) {
    if (!enableMinerFund) {
        return {};
    }

    if (pindexPrev == nullptr) {
        return {};
    }

    const int64_t blockTime = pindexPrev->GetMedianTimePast();
    const int64_t blockHeight = pindexPrev->nHeight + 1;

    // Check for Bodhi upgrades, from the latest to the Bodhi genesis
    if (!params.bodhiActivationTimes.empty()) {
        // Check if blockTime is greater than or equal to the first Bodhi upgrade
        if (blockTime >= params.bodhiActivationTimes[0]) {
            for (size_t bodhiIndex = params.bodhiActivationTimes.size(); bodhiIndex > 0; --bodhiIndex) {
                size_t index = bodhiIndex - 1; // Adjust for zero-based indexing
                
                if (blockTime >= params.bodhiActivationTimes[index]) {
                    if (!params.coinbasePayoutAddresses.bodhiUpgrades.empty() && 
                        index < params.coinbasePayoutAddresses.bodhiUpgrades.size()) {
                        const auto &bodhiAddresses = params.coinbasePayoutAddresses.bodhiUpgrades[index];
                        if (!bodhiAddresses.empty()) {
                            return BuildOutputsCyclingCapped(bodhiAddresses, pindexPrev, blockReward, params, index);
                        }
                    }
                }
            }
        }
        // If the blockTime is before the first Bodhi upgrade, or no Bodhi upgrade applies, proceed to check earlier upgrades
    }

    // 2024-12-21T09:20:00.000Z protocol upgrade which send the miner fund to a burn address
    if (IsRuthEnabled(params, pindexPrev)) {
        Amount burnAmount = blockReward / 2;
        return {BuildBurnOutput(burnAmount)};
    }

    if (IsJudgesEnabled(params, pindexPrev)) {
        return BuildOutputsCycling(params.coinbasePayoutAddresses.judges,
                                   pindexPrev, blockReward);
    }

    if (IsJoshuaEnabled(params, pindexPrev)) {
        return BuildOutputsCycling(params.coinbasePayoutAddresses.joshua,
                                   pindexPrev, blockReward);
    }

    if (IsDeuteronomyEnabled(params, pindexPrev)) {
        return BuildOutputsCycling(params.coinbasePayoutAddresses.deuteronomy,
                                   pindexPrev, blockReward);
    }

    if (IsNumbersEnabled(params, pindexPrev)) {
        return BuildOutputsCycling(params.coinbasePayoutAddresses.numbers,
                                   pindexPrev, blockReward);
    }

    if (IsLeviticusEnabled(params, pindexPrev)) {
        return BuildOutputsFanOut(params.coinbasePayoutAddresses.leviticus,
                                  pindexPrev, blockReward);
    }

    if (IsExodusEnabled(params, pindexPrev)) {
        return BuildOutputsFanOut(params.coinbasePayoutAddresses.exodus,
                                  pindexPrev, blockReward);
    }

    return BuildOutputsFanOut(params.coinbasePayoutAddresses.genesis,
                              pindexPrev, blockReward);
}
