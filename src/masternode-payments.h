// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2018 The PIVX developers
// Copyright (c) 2019-2020 The ZNN developers
// Copyright (c) 2021-2022 The LenoCore developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef MASTERNODE_PAYMENTS_H
#define MASTERNODE_PAYMENTS_H

#include "key.h"
#include "main.h"
#include "masternode.h"
#include <boost/lexical_cast.hpp>

using namespace std;

extern CCriticalSection cs_vecPayments;
extern CCriticalSection cs_mapMasternodeBlocks;
extern CCriticalSection cs_mapMasternodePayeeVotes;

class CMasternodePayments;
class CMasternodePaymentWinner;
class CMasternodeBlockPayees;

extern CMasternodePayments masternodePayments;

#define MNPAYMENTS_SIGNATURES_REQUIRED 6
#define MNPAYMENTS_SIGNATURES_TOTAL 10

class CPaymentWinner {
    public:
        std::string strAddress;
        uint64_t nVotes;
        unsigned int masternodeLevel;
};

void ProcessMessageMasternodePayments(CNode* pfrom, std::string& strCommand, CDataStream& vRecv);
bool IsBlockPayeeValid(const CBlock& block, int nBlockHeight);
std::vector<CPaymentWinner> GetRequiredPayments (int nBlockHeight);
bool IsBlockValueValid(const CBlock& block, CAmount nExpectedValue, CAmount nMinted);
void FillBlockPayee(CMutableTransaction& txNew, CAmount nFees, bool fProofOfStake, bool fZLENOStake);

void DumpMasternodePayments();

/** Save Masternode Payment Data (mnpayments.dat)
 */
class CMasternodePaymentDB
{
private:
    boost::filesystem::path pathDB;
    std::string strMagicMessage;

public:
    enum ReadResult {
        Ok,
        FileError,
        HashReadError,
        IncorrectHash,
        IncorrectMagicMessage,
        IncorrectMagicNumber,
        IncorrectFormat
    };

    CMasternodePaymentDB();
    bool Write(const CMasternodePayments& objToSave);
    ReadResult Read(CMasternodePayments& objToLoad, bool fDryRun = false);
};

class CMasternodePayee
{
public:
    CScript scriptPubKey;
    unsigned int masternodeLevel;
    int nVotes;

    CMasternodePayee()
    {
        scriptPubKey = CScript();
        masternodeLevel = 0;
        nVotes = 0;
    }

    CMasternodePayee(CScript payee, int nVotesIn)
    {
        scriptPubKey = payee;
        masternodeLevel = 0;
        nVotes = nVotesIn;
    }
    
    CMasternodePayee(CScript payee, unsigned int nLevel, int nVotesIn) {
        scriptPubKey = payee;
        masternodeLevel = nLevel;
        nVotes = nVotesIn;
    }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion)
    {
        READWRITE(scriptPubKey);
        READWRITE (masternodeLevel);
        READWRITE(nVotes);
    }
};

// Keep track of votes for payees from masternodes
class CMasternodeBlockPayees
{
public:
    int nBlockHeight;
    std::vector<CMasternodePayee> vecPayments;

    CMasternodeBlockPayees()
    {
        nBlockHeight = 0;
        vecPayments.clear();
    }
    CMasternodeBlockPayees(int nBlockHeightIn)
    {
        nBlockHeight = nBlockHeightIn;
        vecPayments.clear();
    }

    void AddPayee(CScript payeeIn, int nIncrement)
    {
        LOCK(cs_vecPayments);

        BOOST_FOREACH (CMasternodePayee& payee, vecPayments) {
            if (payee.scriptPubKey == payeeIn) {
                payee.nVotes += nIncrement;
                return;
            }
        }

        CMasternodePayee c(payeeIn, nIncrement);
        vecPayments.push_back(c);
    }
    
    void AddPayee (CScript payeeIn, unsigned int masternodeLevel, int nIncrement) {
        LOCK (cs_vecPayments);
        
        BOOST_FOREACH (CMasternodePayee& payee, vecPayments) {
            if ((payee.scriptPubKey == payeeIn) &&
                (payee.masternodeLevel == masternodeLevel)) {
                payee.nVotes += nIncrement;
                
                return;
            }
        }
        
        CMasternodePayee c (payeeIn, masternodeLevel, nIncrement);
        vecPayments.push_back (c);
    }

    bool GetPayee(CScript& payee)
    {
        LOCK(cs_vecPayments);

        int nVotes = -1;
        BOOST_FOREACH (CMasternodePayee& p, vecPayments) {
            if (p.nVotes > nVotes) {
                payee = p.scriptPubKey;
                nVotes = p.nVotes;
            }
        }

        return (nVotes > -1);
    }
    
    bool GetPayee (CScript& payee, unsigned int masternodeLevel) {
        LOCK (cs_vecPayments);
        
        int nVotes = -1;
        
        BOOST_FOREACH (CMasternodePayee& p, vecPayments) {
            if ((p.masternodeLevel == masternodeLevel) &&
                (p.nVotes > nVotes)) {
                payee = p.scriptPubKey;
                nVotes = p.nVotes;
            }
        }
        
        return (nVotes > -1);
    }

    bool HasPayeeWithVotes(CScript payee, int nVotesReq)
    {
        LOCK(cs_vecPayments);

        BOOST_FOREACH (CMasternodePayee& p, vecPayments) {
            if (p.nVotes >= nVotesReq && p.scriptPubKey == payee) return true;
        }

        return false;
    }

    bool IsTransactionValid(const CTransaction& txNew);
    std::vector<CPaymentWinner> GetRequiredPayments ();

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion)
    {
        READWRITE(nBlockHeight);
        READWRITE(vecPayments);
    }
};

// for storing the winning payments
class CMasternodePaymentWinner
{
public:
    CTxIn vinMasternode;

    int nBlockHeight;
    CScript payee; // REMOVE when MIN_VERSION is at least MIN_PEER_PROTO_VERSION_MNW_VIN
    CTxIn vinPayee;
    std::vector<unsigned char> vchSig;

    CMasternodePaymentWinner()
    {
        nBlockHeight = 0;
        vinMasternode = CTxIn();
        payee = CScript(); // REMOVE when MIN_VERSION is at least MIN_PEER_PROTO_VERSION_MNW_VIN
        vinPayee = CTxIn ();
    }

    CMasternodePaymentWinner(CTxIn vinIn)
    {
        nBlockHeight = 0;
        vinMasternode = vinIn;
        payee = CScript(); // REMOVE when MIN_VERSION is at least MIN_PEER_PROTO_VERSION_MNW_VIN
        vinPayee = CTxIn ();
    }

    uint256 GetHash()
    {
        CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
        
        if (ActiveProtocol () < MIN_PEER_PROTO_VERSION_MNW_VIN)
            ss << payee; // REMOVE when MIN_VERSION is at least MIN_PEER_PROTO_VERSION_MNW_VIN
        else
            ss << vinPayee;
        
        ss << nBlockHeight;
        ss << vinMasternode.prevout;

        return ss.GetHash();
    }
    
    unsigned int GetPhase ();
    CScript GetPayeeScript ();
    unsigned int GetPayeePhase ();
    bool Sign(CKey& keyMasternode, CPubKey& pubKeyMasternode);
    bool IsValid(CNode* pnode, std::string& strError);
    bool SignatureValid();
    void Relay();

    void AddPayee (CTxIn vinPayeeIn) {
        vinPayee = vinPayeeIn;
        payee = GetPayeeScript (); // REMOVE when MIN_VERSION is at least MIN_PEER_PROTO_VERSION_MNW_VIN
    }


    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion)
    {
        READWRITE(vinMasternode);
        READWRITE(nBlockHeight);
        
        if (nVersion < MIN_PEER_PROTO_VERSION_MNW_VIN)
            READWRITE (payee); // REMOVE when MIN_VERSION is at least MIN_PEER_PROTO_VERSION_MNW_VIN
        else
            READWRITE (vinPayee);
        
        READWRITE(vchSig);
    }

    std::string ToString()
    {
        std::string ret = "";
        ret += vinMasternode.ToString();
        ret += ", " + boost::lexical_cast<std::string>(nBlockHeight);
        ret += ", " + GetPayeeScript ().ToString () + "@" + boost::lexical_cast<std::string>(GetPayeePhase ());
        ret += ", " + boost::lexical_cast<std::string>((int)vchSig.size());
        return ret;
    }
};

//
// Masternode Payments Class
// Keeps track of who should get paid for which blocks
//

class CMasternodePayments
{
private:
    int nSyncedFromPeer;
    int nLastBlockHeight;

public:
    std::map<uint256, CMasternodePaymentWinner> mapMasternodePayeeVotes;
    std::map<int, CMasternodeBlockPayees> mapMasternodeBlocks;
    std::map<uint256, int> mapMasternodesLastVote; //prevout.hash + prevout.n, nBlockHeight

    CMasternodePayments()
    {
        nSyncedFromPeer = 0;
        nLastBlockHeight = 0;
    }

    void Clear()
    {
        LOCK2(cs_mapMasternodeBlocks, cs_mapMasternodePayeeVotes);
        mapMasternodeBlocks.clear();
        mapMasternodePayeeVotes.clear();
    }

    bool AddWinningMasternode(CMasternodePaymentWinner& winner);
    bool ProcessBlock(int nBlockHeight);

    void Sync(CNode* node, int nCountNeeded);
    void CleanPaymentList();
    int LastPayment(CMasternode& mn);

    bool GetBlockPayee(int nBlockHeight, CScript& payee);
    bool GetBlockPayee (int nBlockHeight, unsigned mnlevel, CScript& payee);
    bool IsTransactionValid(const CTransaction& txNew, int nBlockHeight);
    bool IsScheduled(CMasternode& mn, int nNotBlockHeight);
    bool IsScheduled (CMasternode& mn, int mnLevelCount, int nNotBlockHeight);

    bool CanVote(COutPoint outMasternode, int nBlockHeight)
    {
        LOCK(cs_mapMasternodePayeeVotes);

        if (mapMasternodesLastVote.count(outMasternode.hash + outMasternode.n)) {
            if (mapMasternodesLastVote[outMasternode.hash + outMasternode.n] == nBlockHeight) {
                return false;
            }
        }

        //record this masternode voted
        mapMasternodesLastVote[outMasternode.hash + outMasternode.n] = nBlockHeight;
        return true;
    }
    
    bool CanVote (CMasternodePaymentWinner winner) {
        unsigned int voteForPhase = winner.GetPayeePhase ();
        COutPoint outMasternode = winner.vinMasternode.prevout;
        int nBlockHeight = winner.nBlockHeight;
        
        LOCK (cs_mapMasternodePayeeVotes);
        
        uint256 key = ((outMasternode.hash + outMasternode.n) << 4) + voteForPhase;
        
        if (mapMasternodesLastVote.count (key)) {
            if (mapMasternodesLastVote [key] == nBlockHeight)
                return false;
        }
        
        mapMasternodesLastVote [key] = nBlockHeight;
        
        return true;
    }

    int GetMinMasternodePaymentsProto();
    void ProcessMessageMasternodePayments(CNode* pfrom, std::string& strCommand, CDataStream& vRecv);
    std::vector<CPaymentWinner> GetRequiredPayments (int nBlockHeight);
    void FillBlockPayee(CMutableTransaction& txNew, int64_t nFees, bool fProofOfStake, bool fZLENOStake);
    std::string ToString() const;
    int GetOldestBlock();
    int GetNewestBlock();

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion)
    {
        READWRITE(mapMasternodePayeeVotes);
        READWRITE(mapMasternodeBlocks);
    }
};


#endif
