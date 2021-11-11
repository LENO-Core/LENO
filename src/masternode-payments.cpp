// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2018 The PIVX developers
// Copyright (c) 2019-2020 The ZNN developers
// Copyright (c) 2021-2022 The LenoCore developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "masternode-payments.h"
#include "addrman.h"
#include "chainparams.h"
#include "masternode-budget.h"
#include "masternode-sync.h"
#include "masternodeman.h"
#include "obfuscation.h"
#include "spork.h"
#include "sync.h"
#include "util.h"
#include "utilmoneystr.h"
#include <boost/filesystem.hpp>

/** Object for who's going to get paid on which blocks */
CMasternodePayments masternodePayments;

CCriticalSection cs_vecPayments;
CCriticalSection cs_mapMasternodeBlocks;
CCriticalSection cs_mapMasternodePayeeVotes;

//
// CMasternodePaymentDB
//

CMasternodePaymentDB::CMasternodePaymentDB()
{
    pathDB = GetDataDir() / "mnpayments.dat";
    strMagicMessage = "MasternodePayments";
}

bool CMasternodePaymentDB::Write(const CMasternodePayments& objToSave)
{
    int64_t nStart = GetTimeMillis();

    // serialize, checksum data up to that point, then append checksum
    CDataStream ssObj(SER_DISK, CLIENT_VERSION);
    ssObj << strMagicMessage;                   // masternode cache file specific magic message
    ssObj << FLATDATA(Params().MessageStart()); // network specific magic number
    ssObj << objToSave;
    uint256 hash = Hash(ssObj.begin(), ssObj.end());
    ssObj << hash;

    // open output file, and associate with CAutoFile
    FILE* file = fopen(pathDB.string().c_str(), "wb");
    CAutoFile fileout(file, SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull())
        return error("%s : Failed to open file %s", __func__, pathDB.string());

    // Write and commit header, data
    try {
        fileout << ssObj;
    } catch (std::exception& e) {
        return error("%s : Serialize or I/O error - %s", __func__, e.what());
    }
    fileout.fclose();

    LogPrint("masternode","Written info to mnpayments.dat  %dms\n", GetTimeMillis() - nStart);

    return true;
}

CMasternodePaymentDB::ReadResult CMasternodePaymentDB::Read(CMasternodePayments& objToLoad, bool fDryRun)
{
    int64_t nStart = GetTimeMillis();
    // open input file, and associate with CAutoFile
    FILE* file = fopen(pathDB.string().c_str(), "rb");
    CAutoFile filein(file, SER_DISK, CLIENT_VERSION);
    if (filein.IsNull()) {
        error("%s : Failed to open file %s", __func__, pathDB.string());
        return FileError;
    }

    // use file size to size memory buffer
    int fileSize = boost::filesystem::file_size(pathDB);
    int dataSize = fileSize - sizeof(uint256);
    // Don't try to resize to a negative number if file is small
    if (dataSize < 0)
        dataSize = 0;
    vector<unsigned char> vchData;
    vchData.resize(dataSize);
    uint256 hashIn;

    // read data and checksum from file
    try {
        filein.read((char*)&vchData[0], dataSize);
        filein >> hashIn;
    } catch (std::exception& e) {
        error("%s : Deserialize or I/O error - %s", __func__, e.what());
        return HashReadError;
    }
    filein.fclose();

    CDataStream ssObj(vchData, SER_DISK, CLIENT_VERSION);

    // verify stored checksum matches input data
    uint256 hashTmp = Hash(ssObj.begin(), ssObj.end());
    if (hashIn != hashTmp) {
        error("%s : Checksum mismatch, data corrupted", __func__);
        return IncorrectHash;
    }

    unsigned char pchMsgTmp[4];
    std::string strMagicMessageTmp;
    try {
        // de-serialize file header (masternode cache file specific magic message) and ..
        ssObj >> strMagicMessageTmp;

        // ... verify the message matches predefined one
        if (strMagicMessage != strMagicMessageTmp) {
            error("%s : Invalid masternode payement cache magic message", __func__);
            return IncorrectMagicMessage;
        }


        // de-serialize file header (network specific magic number) and ..
        ssObj >> FLATDATA(pchMsgTmp);

        // ... verify the network matches ours
        if (memcmp(pchMsgTmp, Params().MessageStart(), sizeof(pchMsgTmp))) {
            error("%s : Invalid network magic number", __func__);
            return IncorrectMagicNumber;
        }

        // de-serialize data into CMasternodePayments object
        ssObj >> objToLoad;
    } catch (std::exception& e) {
        objToLoad.Clear();
        error("%s : Deserialize or I/O error - %s", __func__, e.what());
        return IncorrectFormat;
    }

    LogPrint("masternode","Loaded info from mnpayments.dat  %dms\n", GetTimeMillis() - nStart);
    LogPrint("masternode","  %s\n", objToLoad.ToString());
    if (!fDryRun) {
        LogPrint("masternode","Masternode payments manager - cleaning....\n");
        objToLoad.CleanPaymentList();
        LogPrint("masternode","Masternode payments manager - result:\n");
        LogPrint("masternode","  %s\n", objToLoad.ToString());
    }

    return Ok;
}

void DumpMasternodePayments()
{
    int64_t nStart = GetTimeMillis();

    CMasternodePaymentDB paymentdb;
    CMasternodePayments tempPayments;

    LogPrint("masternode","Verifying mnpayments.dat format...\n");
    CMasternodePaymentDB::ReadResult readResult = paymentdb.Read(tempPayments, true);
    // there was an error and it was not an error on file opening => do not proceed
    if (readResult == CMasternodePaymentDB::FileError)
        LogPrint("masternode","Missing budgets file - mnpayments.dat, will try to recreate\n");
    else if (readResult != CMasternodePaymentDB::Ok) {
        LogPrint("masternode","Error reading mnpayments.dat: ");
        if (readResult == CMasternodePaymentDB::IncorrectFormat)
            LogPrint("masternode","magic is ok but data has invalid format, will try to recreate\n");
        else {
            LogPrint("masternode","file format is unknown or invalid, please fix it manually\n");
            return;
        }
    }
    LogPrint("masternode","Writting info to mnpayments.dat...\n");
    paymentdb.Write(masternodePayments);

    LogPrint("masternode","Budget dump finished  %dms\n", GetTimeMillis() - nStart);
}

bool IsBlockValueValid(const CBlock& block, CAmount nExpectedValue, CAmount nMinted)
{
    CBlockIndex* pindexPrev = chainActive.Tip();
    if (pindexPrev == NULL) return true;

    int nHeight = 0;
    if (pindexPrev->GetBlockHash() == block.hashPrevBlock) {
        nHeight = pindexPrev->nHeight + 1;
    } else { //out of order
        BlockMap::iterator mi = mapBlockIndex.find(block.hashPrevBlock);
        if (mi != mapBlockIndex.end() && (*mi).second)
            nHeight = (*mi).second->nHeight + 1;
    }

    if (nHeight == 0) {
        LogPrint("masternode","IsBlockValueValid() : WARNING: Couldn't find previous block\n");
    }

    //LogPrintf("XX69----------> IsBlockValueValid(): nMinted: %d, nExpectedValue: %d\n", FormatMoney(nMinted), FormatMoney(nExpectedValue));

    if (!masternodeSync.IsSynced()) { //there is no budget data to use to check anything
        //super blocks will always be on these blocks, max 100 per budgeting
        if (nHeight % GetBudgetPaymentCycleBlocks() < 100) {
            return true;
        } else {
            if (nMinted > nExpectedValue) {
                return false;
            }
        }
    } else { // we're synced and have data so check the budget schedule

        //are these blocks even enabled
        if (!IsSporkActive(SPORK_13_ENABLE_SUPERBLOCKS)) {
            return nMinted <= nExpectedValue;
        }

        if (budget.IsBudgetPaymentBlock(nHeight)) {
            //the value of the block is evaluated in CheckBlock
            return true;
        } else {
            if (nMinted > nExpectedValue) {
                return false;
            }
        }
    }

    return true;
}

bool IsBlockPayeeValid(const CBlock& block, int nBlockHeight)
{
    TrxValidationStatus transactionStatus = TrxValidationStatus::InValid;

    if (!masternodeSync.IsSynced()) { //there is no budget data to use to check anything -- find the longest chain
        LogPrint("mnpayments", "Client not synced, skipping block payee checks\n");
        return true;
    }

    const CTransaction& txNew = (nBlockHeight > Params().LAST_POW_BLOCK() ? block.vtx[1] : block.vtx[0]);

    //check if it's a budget block
    if (IsSporkActive(SPORK_13_ENABLE_SUPERBLOCKS)) {
        if (budget.IsBudgetPaymentBlock(nBlockHeight)) {
            transactionStatus = budget.IsTransactionValid(txNew, nBlockHeight);
            if (transactionStatus == TrxValidationStatus::Valid) {
                return true;
            }

            if (transactionStatus == TrxValidationStatus::InValid) {
                LogPrint("masternode","Invalid budget payment detected %s\n", txNew.ToString().c_str());
                if (IsSporkActive(SPORK_9_MASTERNODE_BUDGET_ENFORCEMENT))
                    return false;

                LogPrint("masternode","Budget enforcement is disabled, accepting block\n");
            }
        }
    }

    // If we end here the transaction was either TrxValidationStatus::InValid and Budget enforcement is disabled, or
    // a double budget payment (status = TrxValidationStatus::DoublePayment) was detected, or no/not enough masternode
    // votes (status = TrxValidationStatus::VoteThreshold) for a finalized budget were found
    // In all cases a masternode will get the payment for this block

    //check for masternode payee
    if (masternodePayments.IsTransactionValid(txNew, nBlockHeight))
        return true;
    LogPrint("masternode","Invalid mn payment detected %s\n", txNew.ToString().c_str());

    if (IsSporkActive(SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT))
        return false;
    LogPrint("masternode","Masternode payment enforcement is disabled, accepting block\n");

    return true;
}


void FillBlockPayee(CMutableTransaction& txNew, CAmount nFees, bool fProofOfStake, bool fZLENOStake)
{
    CBlockIndex* pindexPrev = chainActive.Tip();
    if (!pindexPrev) return;

    if (IsSporkActive(SPORK_13_ENABLE_SUPERBLOCKS) && budget.IsBudgetPaymentBlock(pindexPrev->nHeight + 1)) {
        budget.FillBlockPayee(txNew, nFees, fProofOfStake);
    } else {
        masternodePayments.FillBlockPayee(txNew, nFees, fProofOfStake, fZLENOStake);
    }
}

std::vector<CPaymentWinner>  GetRequiredPayments (int nBlockHeight) {
    if (IsSporkActive (SPORK_13_ENABLE_SUPERBLOCKS) && budget.IsBudgetPaymentBlock (nBlockHeight))
        return budget.GetRequiredPayments (nBlockHeight);
    
    return masternodePayments.GetRequiredPayments (nBlockHeight);
}

void CMasternodePayments::FillBlockPayee(CMutableTransaction& txNew, int64_t nFees, bool fProofOfStake, bool fZLENOStake)
{
    CBlockIndex* pindexPrev = chainActive.Tip();
    if (!pindexPrev) return;

    bool hasPayment = true;
    CScript payee;
    
    for (unsigned int masternodePhase = 1; masternodePhase <= Params ().getMasternodePhaseCount (pindexPrev->nHeight + 1); masternodePhase++) {
        hasPayment = true;
        
        if (!masternodePayments.GetBlockPayee (pindexPrev->nHeight + 1, masternodePhase, payee)) {
            // No masternode was detected
            CMasternode* winningNode = mnodeman.GetCurrentMasternodeOnLevel (masternodePhase, 1);
            
            if (!winningNode) {
                LogPrint ("masternode", "CreateNewBlock: Failed to detect masternode to pay\n");
                hasPayment = false;
            } else
                payee = GetScriptForDestination (winningNode->pubKeyCollateralAddress.GetID ());
        }

        CAmount blockValue = GetBlockValue (pindexPrev->nHeight);
        CAmount masternodePayment = GetMasternodePayment (pindexPrev->nHeight + 1, masternodePhase, blockValue, 0, fZLENOStake);
        
        if (hasPayment) {
            unsigned int i = txNew.vout.size ();
            txNew.vout.resize (i + 1);
            
            txNew.vout [i].scriptPubKey = payee;
            txNew.vout [i].nValue = masternodePayment;
            
            if (!txNew.vout [fProofOfStake ? 1 : 0].IsZerocoinMint ())
                txNew.vout [fProofOfStake ? 1 : 0].nValue -= masternodePayment;
            
            CTxDestination address1;
            ExtractDestination (payee, address1);
            CBitcoinAddress address2 (address1);
            
            LogPrint ("masternode", "Masternode payment of %s to %s\n", FormatMoney (masternodePayment).c_str (), address2.ToString ().c_str ());
        } else if (!fProofOfStake)
            txNew.vout [0].nValue = blockValue - masternodePayment;
    }
}

int CMasternodePayments::GetMinMasternodePaymentsProto()
{
    if (IsSporkActive(SPORK_10_MASTERNODE_PAY_UPDATED_NODES))
        return ActiveProtocol();                          // Allow only updated peers
    else
        return MIN_PEER_PROTO_VERSION_BEFORE_ENFORCEMENT; // Also allow old peers as long as they are allowed to run
}

void CMasternodePayments::ProcessMessageMasternodePayments(CNode* pfrom, std::string& strCommand, CDataStream& vRecv)
{
    if (!masternodeSync.IsBlockchainSynced()) return;

    if (fLiteMode) return; //disable all Obfuscation/Masternode related functionality


    if (strCommand == "mnget") { //Masternode Payments Request Sync
        if (fLiteMode) return;   //disable all Obfuscation/Masternode related functionality

        int nCountNeeded;
        vRecv >> nCountNeeded;

        if (Params().NetworkID() == CBaseChainParams::MAIN) {
            if (pfrom->HasFulfilledRequest("mnget")) {
                LogPrintf("CMasternodePayments::ProcessMessageMasternodePayments() : mnget - peer already asked me for the list\n");
                Misbehaving(pfrom->GetId(), 20);
                return;
            }
        }

        pfrom->FulfilledRequest("mnget");
        masternodePayments.Sync(pfrom, nCountNeeded);
        LogPrint("mnpayments", "mnget - Sent Masternode winners to peer %i\n", pfrom->GetId());
    } else if (strCommand == "mnw") { //Masternode Payments Declare Winner
        //this is required in litemodef
        CMasternodePaymentWinner winner;
        vRecv >> winner;

        if (pfrom->nVersion < ActiveProtocol()) return;
        
        if (winner.GetPhase () == 0) {
            LogPrint ("mnpayments", "CMasternodePayments::ProcessMessageMasternodePayments() : mnw - Could not find phase of masternode \n");
            
            if (masternodeSync.IsSynced ())
                Misbehaving (pfrom->GetId (), 20);
            
            mnodeman.AskForMN (pfrom, winner.vinMasternode);
            
            return;
        }
        
        unsigned int payeePhase = winner.GetPayeePhase ();
        
        if (payeePhase == 0) {
            LogPrint ("mnpayments", "CMasternodePayments::ProcessMessageMasternodePayments() : mnw - Could not find phase of payee %s\n", winner.GetPayeeScript ().ToAddressString ());
            
            // if (masternodeSync.IsSynced ())
            //    Misbehaving (pfrom->GetId (), 20);
            
            if (ActiveProtocol () >= MIN_PEER_PROTO_VERSION_MNW_VIN)
                mnodeman.AskForMN (pfrom, winner.vinPayee);
            
            return;
        }

        int nHeight;
        {
            TRY_LOCK(cs_main, locked);
            if (!locked || chainActive.Tip() == NULL) return;
            nHeight = chainActive.Tip()->nHeight;
        }
        
        CTxDestination masternodeAddress;
        ExtractDestination (winner.payee, masternodeAddress);
        CBitcoinAddress payee_addr (masternodeAddress);

        if (masternodePayments.mapMasternodePayeeVotes.count(winner.GetHash())) {
            LogPrint("mnpayments", "mnw - Already seen - %s bestHeight %d\n", winner.GetHash().ToString().c_str(), nHeight);
            masternodeSync.AddedMasternodeWinner(winner.GetHash());
            return;
        }

        int nFirstBlock = nHeight - int (mnodeman.CountEnabledOnLevel (payeePhase) * 1.25);
        
        if (winner.nBlockHeight < nFirstBlock || winner.nBlockHeight > nHeight + 20) {
            LogPrint("mnpayments", "mnw - winner out of range - FirstBlock %d Height %d bestHeight %d\n", nFirstBlock, winner.nBlockHeight, nHeight);
            return;
        }

        std::string strError = "";
        if (!winner.IsValid(pfrom, strError)) {
            // if(strError != "") LogPrint("masternode","mnw - invalid message - %s\n", strError);
            return;
        }

        if (!masternodePayments.CanVote  (winner)) {
            //  LogPrint("masternode","mnw - masternode already voted - %s\n", winner.vinMasternode.prevout.ToStringShort());
            return;
        }

        if (!winner.SignatureValid()) {
            if (masternodeSync.IsSynced()) {
                LogPrintf("CMasternodePayments::ProcessMessageMasternodePayments() : mnw - invalid signature\n");
                Misbehaving(pfrom->GetId(), 20);
            }
            // it could just be a non-synced masternode
            mnodeman.AskForMN(pfrom, winner.vinMasternode);
            return;
        }
        
        if (masternodePayments.AddWinningMasternode(winner)) {
            winner.Relay();
            masternodeSync.AddedMasternodeWinner(winner.GetHash());
        }
    }
}

unsigned int CMasternodePaymentWinner::GetPhase () {
    CMasternode* pmn = mnodeman.Find (vinMasternode);
    
    if (pmn)
        return pmn->GetPhase (nBlockHeight);
    
    return 0;
}

CScript CMasternodePaymentWinner::GetPayeeScript () {
    if (ActiveProtocol () < MIN_PEER_PROTO_VERSION_MNW_VIN) {
        if (payee == CScript ())
            LogPrint ("mnpayments", "CMasternodePaymentWinner::GetPayeeScript() : returning empty CScript\n");
        
        return payee;
    }
    
    CMasternode* pmn = mnodeman.Find (vinPayee);
    
    if (pmn != NULL)
        return GetScriptForDestination (pmn->pubKeyCollateralAddress.GetID ());
    
    CTransaction prevTx;
    uint256 hashBlock = 0;
    
    if (GetTransaction (vinPayee.prevout.hash, prevTx, hashBlock, true))
        return prevTx.vout [vinPayee.prevout.n].scriptPubKey;
    
    LogPrint ("mnpayments", "CMasternodePaymentWinner::GetPayeeScript() : Failed to get payee's CScript\n");
    
    return CScript ();
}

unsigned int CMasternodePaymentWinner::GetPayeePhase () {
    CMasternode* pmn;
    
    if (ActiveProtocol () < MIN_PEER_PROTO_VERSION_MNW_VIN)
        pmn = mnodeman.Find (payee); // REMOVE when MIN_VERSION is at least MIN_PEER_PROTO_VERSION_MNW_VIN
    else
        pmn = mnodeman.Find (vinPayee);
    
    if (pmn)
        return pmn->GetPhase (nBlockHeight);
    
    return 0;
}

bool CMasternodePaymentWinner::Sign(CKey& keyMasternode, CPubKey& pubKeyMasternode)
{
    std::string errorMessage;
    std::string strMasterNodeSignMessage;

    std::string strMessage = vinMasternode.prevout.ToStringShort () + std::to_string (nBlockHeight) + GetPayeeScript ().ToString ();

    if (!obfuScationSigner.SignMessage(strMessage, errorMessage, vchSig, keyMasternode)) {
        LogPrint("masternode","CMasternodePing::Sign() - Error: %s\n", errorMessage.c_str());
        return false;
    }

    if (!obfuScationSigner.VerifyMessage(pubKeyMasternode, vchSig, strMessage, errorMessage)) {
        LogPrint("masternode","CMasternodePing::Sign() - Error: %s\n", errorMessage.c_str());
        return false;
    }

    return true;
}

bool CMasternodePayments::GetBlockPayee(int nBlockHeight, CScript& payee)
{
    if (mapMasternodeBlocks.count(nBlockHeight)) {
        return mapMasternodeBlocks[nBlockHeight].GetPayee(payee);
    }

    return false;
}

bool CMasternodePayments::GetBlockPayee (int nBlockHeight, unsigned mnLevel, CScript& payee) {
    if (mapMasternodeBlocks.count (nBlockHeight))
        return mapMasternodeBlocks [nBlockHeight].GetPayee (payee, mnLevel);
    
    return false;
}

// Is this masternode scheduled to get paid soon?
// -- Only look ahead up to 8 blocks to allow for propagation of the latest 2 winners
bool CMasternodePayments::IsScheduled(CMasternode& mn, int nNotBlockHeight)
{
    LOCK(cs_mapMasternodeBlocks);

    int nHeight;
    {
        TRY_LOCK(cs_main, locked);
        if (!locked || chainActive.Tip() == NULL) return false;
        nHeight = chainActive.Tip()->nHeight;
    }

    CScript mnpayee;
    mnpayee = GetScriptForDestination(mn.pubKeyCollateralAddress.GetID());

    CScript payee;
    for (int64_t h = nHeight; h <= nHeight + 8; h++) {
        if (h == nNotBlockHeight) continue;
        if (mapMasternodeBlocks.count(h)) {
            if (mapMasternodeBlocks[h].GetPayee(payee)) {
                if (mnpayee == payee) {
                    return true;
                }
            }
        }
    }

    return false;
}

bool CMasternodePayments::IsScheduled (CMasternode& mn, int mnLevelCount, int nNotBlockHeight) {
    LOCK (cs_mapMasternodeBlocks);
    
    int nHeight;
    
    {
        TRY_LOCK (cs_main, locked);
        
        if (!locked || (chainActive.Tip () == NULL))
            return false;
        
        nHeight = chainActive.Tip ()->nHeight;
    }
    
    CScript mnPayee;
    CScript payee;
    unsigned int masternodeLevel = mn.GetPhase ();
    
    mnPayee = GetScriptForDestination (mn.pubKeyCollateralAddress.GetID());
    
    for (int h_upper_bound = nHeight + 10, h = h_upper_bound - std::min (10, mnLevelCount - 1); h < h_upper_bound; ++h) {
        if (h == nNotBlockHeight)
            continue;
        
        if (!mapMasternodeBlocks.count(h))
            continue;
        
        if (!mapMasternodeBlocks [h].GetPayee (payee, masternodeLevel))
            continue;
        
        if (mnPayee == payee)
            return true;
        
    }
    
    return false;
}

bool CMasternodePayments::AddWinningMasternode(CMasternodePaymentWinner& winnerIn)
{
    uint256 blockHash = 0;
    if (!GetBlockHash(blockHash, winnerIn.nBlockHeight - 100)) {
        return false;
    }

    {
        LOCK2(cs_mapMasternodePayeeVotes, cs_mapMasternodeBlocks);

        if (mapMasternodePayeeVotes.count(winnerIn.GetHash())) {
            return false;
        }

        mapMasternodePayeeVotes[winnerIn.GetHash()] = winnerIn;

        if (!mapMasternodeBlocks.count(winnerIn.nBlockHeight)) {
            CMasternodeBlockPayees blockPayees(winnerIn.nBlockHeight);
            mapMasternodeBlocks[winnerIn.nBlockHeight] = blockPayees;
        }
    }

    mapMasternodeBlocks [winnerIn.nBlockHeight].AddPayee (winnerIn.GetPayeeScript (), winnerIn.GetPayeePhase (), 1);

    return true;
}

bool CMasternodeBlockPayees::IsTransactionValid(const CTransaction& txNew)
{
    LOCK(cs_vecPayments);

    int nMasternode_Drift_Count = 0;
    CAmount nReward = GetBlockValue(nBlockHeight);

    if (IsSporkActive(SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT)) {
        // Get a stable number of masternodes by ignoring newly activated (< 8000 sec old) masternodes
        nMasternode_Drift_Count = mnodeman.stable_size() + Params().MasternodeCountDrift();
    }
    else {
        //account for the fact that all peers do not see the same masternode count. A allowance of being off our masternode count is given
        //we only need to look at an increased masternode count because as count increases, the reward decreases. This code only checks
        //for mnPayment >= required, so it only makes sense to check the max node count allowed.
        nMasternode_Drift_Count = mnodeman.size() + Params().MasternodeCountDrift();
    }
    
    bool transactionCorrect = true;
    CAmount requiredMasternodePayment = 0;

    for (unsigned int masternodePhase = 1; masternodePhase <= Params ().getMasternodePhaseCount (nBlockHeight); masternodePhase++) {
        // Require at least 6 signatures
        int nMaxSignatures = 0;
        
        for (CMasternodePayee& payee : vecPayments)
            if ((payee.nVotes >= nMaxSignatures) &&
                (payee.masternodeLevel == masternodePhase))
                nMaxSignatures = payee.nVotes;
        
        // if we don't have at least 6 signatures on a payee, approve whichever is the longest chain
        if (nMaxSignatures < MNPAYMENTS_SIGNATURES_REQUIRED)
            continue;
        
        requiredMasternodePayment = GetMasternodePayment (nBlockHeight, masternodePhase, nReward, nMasternode_Drift_Count, false);
        
        bool hasLevelPayment = false;
        std::string strPayeesPossible = "";
        
        for (CMasternodePayee& payee : vecPayments) {
            // Skip payees on other levels
            if (payee.masternodeLevel != masternodePhase)
                continue;
            
            // Skip payees with little votes
            if (payee.nVotes < MNPAYMENTS_SIGNATURES_REQUIRED)
                continue;
            
            // Check if this payee was paid
            for (CTxOut out : txNew.vout)
                if (payee.scriptPubKey == out.scriptPubKey) {
                    if(out.nValue >= requiredMasternodePayment) {
                        hasLevelPayment = true;
                        LogPrintf ("Found payment of %s on phase %d at %d\n", FormatMoney (out.nValue).c_str (), masternodePhase, nBlockHeight);
                        
                        break;
                    } else
                        LogPrint ("masternode", "Masternode payment is out of drift range. Paid=%s Min=%s\n", FormatMoney (out.nValue).c_str (), FormatMoney (requiredMasternodePayment).c_str ());
                }
            
            if (hasLevelPayment)
                break;
            
            // Push payee to possible payees
            CTxDestination address1;
            ExtractDestination (payee.scriptPubKey, address1);
            CBitcoinAddress address2 (address1);
            
            if (strPayeesPossible == "")
                strPayeesPossible += address2.ToString ();
            else
                strPayeesPossible += "," + address2.ToString ();
        }
        
        if (!hasLevelPayment) {
            LogPrint ("masternode", "CMasternodePayments::IsTransactionValid - Missing required payment of %s to %s on level %d\n", FormatMoney (requiredMasternodePayment).c_str (), strPayeesPossible.c_str (), masternodePhase);
            
            transactionCorrect = false;
        }
    }
    
    return transactionCorrect;
}

std::vector<CPaymentWinner> CMasternodeBlockPayees::GetRequiredPayments () {
    LOCK (cs_vecPayments);
    std::vector<CPaymentWinner> vPaymentWinners;
    
    for (CMasternodePayee& payee : vecPayments) {
        CTxDestination address1;
        ExtractDestination (payee.scriptPubKey, address1);
        CBitcoinAddress address2 (address1);
        CPaymentWinner paymentWinner;
        
        paymentWinner.strAddress = address2.ToString ();
        paymentWinner.nVotes = payee.nVotes;
        paymentWinner.masternodeLevel = payee.masternodeLevel;
        
        vPaymentWinners.push_back (paymentWinner);
    }
    
    return vPaymentWinners;
}

std::vector<CPaymentWinner> CMasternodePayments::GetRequiredPayments (int nBlockHeight) {
    LOCK (cs_mapMasternodeBlocks);
    
    if (mapMasternodeBlocks.count (nBlockHeight))
        return mapMasternodeBlocks [nBlockHeight].GetRequiredPayments ();
    
    std::vector<CPaymentWinner> vPaymentWinners;
    
    return vPaymentWinners;
}

bool CMasternodePayments::IsTransactionValid(const CTransaction& txNew, int nBlockHeight)
{
    LOCK(cs_mapMasternodeBlocks);

    if (mapMasternodeBlocks.count(nBlockHeight)) {
        return mapMasternodeBlocks[nBlockHeight].IsTransactionValid(txNew);
    }

    return true;
}

void CMasternodePayments::CleanPaymentList()
{
    LOCK2(cs_mapMasternodePayeeVotes, cs_mapMasternodeBlocks);

    int nHeight;
    {
        TRY_LOCK(cs_main, locked);
        if (!locked || chainActive.Tip() == NULL) return;
        nHeight = chainActive.Tip()->nHeight;
    }

    //keep up to five cycles for historical sake
    int nLimit = std::max(int(mnodeman.size() * 1.25), 1000);

    std::map<uint256, CMasternodePaymentWinner>::iterator it = mapMasternodePayeeVotes.begin();
    while (it != mapMasternodePayeeVotes.end()) {
        CMasternodePaymentWinner winner = (*it).second;

        if (nHeight - winner.nBlockHeight > nLimit) {
            LogPrint("mnpayments", "CMasternodePayments::CleanPaymentList - Removing old Masternode payment - block %d\n", winner.nBlockHeight);
            masternodeSync.mapSeenSyncMNW.erase((*it).first);
            mapMasternodePayeeVotes.erase(it++);
            mapMasternodeBlocks.erase(winner.nBlockHeight);
        } else {
            ++it;
        }
    }
}

bool CMasternodePaymentWinner::IsValid(CNode* pnode, std::string& strError)
{
    CMasternode* pmn = mnodeman.Find(vinMasternode);

    if (!pmn) {
        strError = strprintf("Unknown Masternode %s", vinMasternode.prevout.hash.ToString());
        LogPrint("masternode","CMasternodePaymentWinner::IsValid - %s\n", strError);
        mnodeman.AskForMN(pnode, vinMasternode);
        return false;
    }

    if (pmn->protocolVersion < ActiveProtocol()) {
        strError = strprintf("Masternode protocol too old %d - req %d", pmn->protocolVersion, ActiveProtocol());
        LogPrint("masternode","CMasternodePaymentWinner::IsValid - %s\n", strError);
        return false;
    }

    int n = mnodeman.GetMasternodeRank(vinMasternode, nBlockHeight - 100, ActiveProtocol());

    if (n > MNPAYMENTS_SIGNATURES_TOTAL) {
        //It's common to have masternodes mistakenly think they are in the top 10
        // We don't want to print all of these messages, or punish them unless they're way off
        if (n > MNPAYMENTS_SIGNATURES_TOTAL * 2) {
            strError = strprintf("Masternode not in the top %d (%d)", MNPAYMENTS_SIGNATURES_TOTAL * 2, n);
            LogPrint("masternode","CMasternodePaymentWinner::IsValid - %s\n", strError);
            //if (masternodeSync.IsSynced()) Misbehaving(pnode->GetId(), 20);
        }
        return false;
    }

    return true;
}

bool CMasternodePayments::ProcessBlock(int nBlockHeight)
{
    if (!fMasterNode) return false;

    //reference node - hybrid mode

    int n = mnodeman.GetMasternodeRank(activeMasternode.vin, nBlockHeight - 100, ActiveProtocol());

    if (n == -1) {
        LogPrint("mnpayments", "CMasternodePayments::ProcessBlock - Unknown Masternode\n");
        return false;
    }

    if (n > MNPAYMENTS_SIGNATURES_TOTAL) {
        LogPrint("mnpayments", "CMasternodePayments::ProcessBlock - Masternode not in the top %d (%d)\n", MNPAYMENTS_SIGNATURES_TOTAL, n);
        return false;
    }

    if (nBlockHeight <= nLastBlockHeight) return false;

    if (budget.IsBudgetPaymentBlock (nBlockHeight))
        return true;
    
    LogPrint ("masternode", "CMasternodePayments::ProcessBlock() Start nHeight %d - vin %s. \n", nBlockHeight, activeMasternode.vin.prevout.hash.ToString ());
    
    // Prepare for signing messages
    std::string errorMessage;
    CPubKey pubKeyMasternode;
    CKey keyMasternode;
    
    if (!obfuScationSigner.SetKey (strMasterNodePrivKey, errorMessage, keyMasternode, pubKeyMasternode)) {
        LogPrint ("masternode", "CMasternodePayments::ProcessBlock() - Error upon calling SetKey: %s\n", errorMessage.c_str ());
        
        return false;
    }
    
    for (unsigned int masternodePhase = 1; masternodePhase <= Params ().getMasternodePhaseCount (nBlockHeight + 1); masternodePhase++) {
        // Create a new winner for this level
        CMasternodePaymentWinner newWinner (activeMasternode.vin);
        
        // pay to the oldest MN that still had no payment but its input is old enough and it was active long enough
        int nCount = 0;
        CMasternode* pmn = mnodeman.GetNextMasternodeInQueueForPayment (nBlockHeight + 1, masternodePhase, true, nCount);
        
        if (pmn == NULL) {
            LogPrint ("masternode", "CMasternodePayments::ProcessBlock() Failed to find masternode to pay\n");
            
            continue;
        }
        
        newWinner.nBlockHeight = nBlockHeight;
        newWinner.AddPayee (pmn->vin);
        
        CTxDestination address1;
        ExtractDestination (newWinner.GetPayeeScript (), address1);
        CBitcoinAddress address2 (address1);
        
        LogPrint ("masternode", "CMasternodePayments::ProcessBlock() Winner payee %s nHeight %d level %d. \n", address2.ToString ().c_str (), newWinner.nBlockHeight, masternodePhase);
        
        if (newWinner.Sign (keyMasternode, pubKeyMasternode)) {
            if (AddWinningMasternode (newWinner)) {
                newWinner.Relay ();
                nLastBlockHeight = nBlockHeight;
            } else
                LogPrint ("masternode", "CMasternodePayments::ProcessBlock() FAILED to add winning masternode\n");
        } else
            LogPrint ("masternode", "CMasternodePayments::ProcessBlock() FAILED to sign winner\n");
    }
    
    return (nLastBlockHeight == nBlockHeight);
}

void CMasternodePaymentWinner::Relay()
{
    CInv inv(MSG_MASTERNODE_WINNER, GetHash());
    RelayInv(inv);
}

bool CMasternodePaymentWinner::SignatureValid()
{
    CMasternode* pmn = mnodeman.Find(vinMasternode);

    if (pmn != NULL) {
        std::string strMessage = vinMasternode.prevout.ToStringShort () + std::to_string (nBlockHeight) + GetPayeeScript ().ToString ();

        std::string errorMessage = "";
        if (!obfuScationSigner.VerifyMessage(pmn->pubKeyMasternode, vchSig, strMessage, errorMessage)) {
            return error("CMasternodePaymentWinner::SignatureValid() - Got bad Masternode address signature %s\n", vinMasternode.prevout.hash.ToString());
        }

        return true;
    }

    return false;
}

void CMasternodePayments::Sync(CNode* node, int nCountNeeded)
{
    LOCK(cs_mapMasternodePayeeVotes);

    int nHeight;
    {
        TRY_LOCK(cs_main, locked);
        if (!locked || chainActive.Tip() == NULL) return;
        nHeight = chainActive.Tip()->nHeight;
    }

    int nCount = (mnodeman.CountEnabled() * 1.25);
    if (nCountNeeded > nCount) nCountNeeded = nCount;

    int nInvCount = 0;
    std::map<uint256, CMasternodePaymentWinner>::iterator it = mapMasternodePayeeVotes.begin();
    while (it != mapMasternodePayeeVotes.end()) {
        CMasternodePaymentWinner winner = (*it).second;
        if (winner.nBlockHeight >= nHeight - nCountNeeded && winner.nBlockHeight <= nHeight + 20) {
            node->PushInventory(CInv(MSG_MASTERNODE_WINNER, winner.GetHash()));
            nInvCount++;
        }
        ++it;
    }
    node->PushMessage("ssc", MASTERNODE_SYNC_MNW, nInvCount);
}

std::string CMasternodePayments::ToString() const
{
    std::ostringstream info;

    info << "Votes: " << (int)mapMasternodePayeeVotes.size() << ", Blocks: " << (int)mapMasternodeBlocks.size();

    return info.str();
}


int CMasternodePayments::GetOldestBlock()
{
    LOCK(cs_mapMasternodeBlocks);

    int nOldestBlock = std::numeric_limits<int>::max();

    std::map<int, CMasternodeBlockPayees>::iterator it = mapMasternodeBlocks.begin();
    while (it != mapMasternodeBlocks.end()) {
        if ((*it).first < nOldestBlock) {
            nOldestBlock = (*it).first;
        }
        it++;
    }

    return nOldestBlock;
}


int CMasternodePayments::GetNewestBlock()
{
    LOCK(cs_mapMasternodeBlocks);

    int nNewestBlock = 0;

    std::map<int, CMasternodeBlockPayees>::iterator it = mapMasternodeBlocks.begin();
    while (it != mapMasternodeBlocks.end()) {
        if ((*it).first > nNewestBlock) {
            nNewestBlock = (*it).first;
        }
        it++;
    }

    return nNewestBlock;
}
