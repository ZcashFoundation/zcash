// Copyright (c) 2011-2015 The Bitcoin Core developers
// Copyright (c) 2017-2023 The Zcash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#include "policy/fees.h"
#include "txmempool.h"
#include "uint256.h"
#include "util/system.h"

#include "test/test_bitcoin.h"

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(policyestimator_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(BlockPolicyEstimates)
{
    CTxMemPool mpool(CFeeRate(1000));
    TestMemPoolEntryHelper entry;
    CAmount basefee(2000);
    CAmount deltaFee(100);
    std::vector<CAmount> feeV;

    // Populate vectors of increasing fees
    for (int j = 0; j < 10; j++) {
        feeV.push_back(basefee * (j+1));
    }

    // Store the hashes of transactions that have been
    // added to the mempool by their associate fee
    // txHashes[j] is populated with transactions either of
    // fee = basefee * (j+1)
    std::vector<uint256> txHashes[10];

    // Create a transaction template
    CScript garbage;
    for (unsigned int i = 0; i < 128; i++)
        garbage.push_back('X');
    CMutableTransaction tx;
    std::list<CTransaction> dummyConflicted;
    tx.vin.resize(1);
    tx.vin[0].scriptSig = garbage;
    tx.vout.resize(1);
    tx.vout[0].nValue=0LL;
    CFeeRate baseRate(basefee, ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION));

    // Create a fake block
    std::vector<CTransaction> block;
    int blocknum = 0;

    // Loop through 200 blocks
    // At a decay .998 and 4 fee transactions per block
    // This makes the tx count about 1.33 per bucket, above the 1 threshold
    while (blocknum < 200) {
        for (int j = 0; j < 10; j++) { // For each fee
            for (int k = 0; k < 4; k++) { // add 4 fee txs
                tx.vin[0].prevout.n = 10000*blocknum+100*j+k; // make transaction unique
                uint256 hash = tx.GetHash();
                mpool.addUnchecked(hash, entry.Fee(feeV[j]).Time(GetTime()).Height(blocknum).FromTx(tx, &mpool));
                txHashes[j].push_back(hash);
            }
        }
        //Create blocks where higher fee txs are included more often
        for (int h = 0; h <= blocknum%10; h++) {
            // 10/10 blocks add highest fee transactions
            // 9/10 blocks add 2nd highest and so on until ...
            // 1/10 blocks add lowest fee transactions
            while (txHashes[9-h].size()) {
                std::shared_ptr<const CTransaction> ptx = mpool.get(txHashes[9-h].back());
                if (ptx)
                    block.push_back(*ptx);
                txHashes[9-h].pop_back();
            }
        }
        mpool.removeForBlock(block, ++blocknum, dummyConflicted);
        block.clear();
        if (blocknum == 30) {
            // At this point we should need to combine 5 buckets to get enough data points
            // So estimateFee(1) should fail and estimateFee(2) should return somewhere around
            // 8*baserate
            BOOST_CHECK(mpool.estimateFee(1) == CFeeRate(0));
            BOOST_CHECK(mpool.estimateFee(2).GetFeePerK() < 8*baseRate.GetFeePerK() + deltaFee);
            BOOST_CHECK(mpool.estimateFee(2).GetFeePerK() > 8*baseRate.GetFeePerK() - deltaFee);
        }
    }

    std::vector<CAmount> origFeeEst;
    // Highest feerate is 10*baseRate and gets in all blocks,
    // second highest feerate is 9*baseRate and gets in 9/10 blocks = 90%,
    // third highest feerate is 8*base rate, and gets in 8/10 blocks = 80%,
    // so estimateFee(1) should return 9*baseRate.
    // Third highest feerate has 90% chance of being included by 2 blocks,
    // so estimateFee(2) should return 8*baseRate etc...
    for (int i = 1; i < 10;i++) {
        origFeeEst.push_back(mpool.estimateFee(i).GetFeePerK());
        if (i > 1) { // Fee estimates should be monotonically decreasing
            BOOST_CHECK(origFeeEst[i-1] <= origFeeEst[i-2]);
        }
        BOOST_CHECK(origFeeEst[i-1] < (10-i)*baseRate.GetFeePerK() + deltaFee);
        BOOST_CHECK(origFeeEst[i-1] > (10-i)*baseRate.GetFeePerK() - deltaFee);
    }

    // Mine 50 more blocks with no transactions happening, estimates shouldn't change
    // We haven't decayed the moving average enough so we still have enough data points in every bucket
    while (blocknum < 250)
        mpool.removeForBlock(block, ++blocknum, dummyConflicted);

    for (int i = 1; i < 10;i++) {
        BOOST_CHECK(mpool.estimateFee(i).GetFeePerK() < origFeeEst[i-1] + deltaFee);
        BOOST_CHECK(mpool.estimateFee(i).GetFeePerK() > origFeeEst[i-1] - deltaFee);
    }


    // Mine 15 more blocks with lots of transactions happening and not getting mined
    // Estimates should go up
    while (blocknum < 265) {
        for (int j = 0; j < 10; j++) { // For each fee multiple
            for (int k = 0; k < 4; k++) { // add 4 fee txs
                tx.vin[0].prevout.n = 10000*blocknum+100*j+k;
                uint256 hash = tx.GetHash();
                mpool.addUnchecked(hash, entry.Fee(feeV[j]).Time(GetTime()).Height(blocknum).FromTx(tx, &mpool));
                txHashes[j].push_back(hash);
            }
        }
        mpool.removeForBlock(block, ++blocknum, dummyConflicted);
    }

    for (int i = 1; i < 10;i++) {
        BOOST_CHECK(mpool.estimateFee(i).GetFeePerK() > origFeeEst[i-1] - deltaFee);
    }

    // Mine all those transactions
    // Estimates should still not be below original
    for (int j = 0; j < 10; j++) {
        while(txHashes[j].size()) {
            std::shared_ptr<const CTransaction> ptx = mpool.get(txHashes[j].back());
            if (ptx)
                block.push_back(*ptx);
            txHashes[j].pop_back();
        }
    }
    mpool.removeForBlock(block, 265, dummyConflicted);
    block.clear();
    for (int i = 1; i < 10;i++) {
        BOOST_CHECK(mpool.estimateFee(i).GetFeePerK() > origFeeEst[i-1] - deltaFee);
    }

    // Mine 100 more blocks where everything is mined every block
    // Estimates should be below original estimates (not possible for last estimate)
    while (blocknum < 365) {
        for (int j = 0; j < 10; j++) { // For each fee multiple
            for (int k = 0; k < 4; k++) { // add 4 fee txs
                tx.vin[0].prevout.n = 10000*blocknum+100*j+k;
                uint256 hash = tx.GetHash();
                mpool.addUnchecked(hash, entry.Fee(feeV[j]).Time(GetTime()).Height(blocknum).FromTx(tx, &mpool));
                std::shared_ptr<const CTransaction> ptx = mpool.get(hash);
                if (ptx)
                    block.push_back(*ptx);

            }
        }
        mpool.removeForBlock(block, ++blocknum, dummyConflicted);
        block.clear();
    }
    for (int i = 1; i < 9; i++) {
        BOOST_CHECK(mpool.estimateFee(i).GetFeePerK() < origFeeEst[i-1] - deltaFee);
    }
}


BOOST_AUTO_TEST_CASE(TxConfirmStats_FindBucketIndex)
{
    std::vector<double> buckets {0.0, 3.5, 42.0};
    TxConfirmStats txcs;

    txcs.Initialize(buckets, MAX_BLOCK_CONFIRMS, DEFAULT_DECAY);

    BOOST_CHECK_EQUAL(txcs.FindBucketIndex(-1.0), 0);
    BOOST_CHECK_EQUAL(txcs.FindBucketIndex(0.0), 0);
    BOOST_CHECK_EQUAL(txcs.FindBucketIndex(1.0), 1);
    BOOST_CHECK_EQUAL(txcs.FindBucketIndex(3.5), 1);
    BOOST_CHECK_EQUAL(txcs.FindBucketIndex(4.0), 2);
    BOOST_CHECK_EQUAL(txcs.FindBucketIndex(43.0), 3);
    BOOST_CHECK_EQUAL(txcs.FindBucketIndex(INF_FEERATE), 3);
    BOOST_CHECK_EQUAL(txcs.FindBucketIndex(2.0*INF_FEERATE), 3);
    BOOST_CHECK_EQUAL(txcs.FindBucketIndex(std::numeric_limits<double>::infinity()), 3);
    BOOST_CHECK_EQUAL(txcs.FindBucketIndex(2.0*std::numeric_limits<double>::infinity()), 3);
    BOOST_CHECK_EQUAL(txcs.FindBucketIndex(nan("")), 0);
}

BOOST_AUTO_TEST_SUITE_END()
