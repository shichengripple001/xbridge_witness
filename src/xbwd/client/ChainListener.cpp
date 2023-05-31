//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2021 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <xbwd/client/ChainListener.h>
#include <xbwd/client/RpcResultParse.h>
#include <xbwd/client/WebsocketClient.h>
#include <xbwd/federator/Federator.h>
#include <xbwd/federator/FederatorEvents.h>

#include <ripple/basics/Log.h>
#include <ripple/basics/XRPAmount.h>
#include <ripple/basics/strHex.h>
#include <ripple/json/Output.h>
#include <ripple/json/json_get_or_throw.h>
#include <ripple/json/json_writer.h>
#include <ripple/protocol/AccountID.h>
#include <ripple/protocol/LedgerFormats.h>
#include <ripple/protocol/SField.h>
#include <ripple/protocol/STAmount.h>
#include <ripple/protocol/TER.h>
#include <ripple/protocol/TxFlags.h>
#include <ripple/protocol/jss.h>

#include <charconv>
#include <type_traits>

namespace xbwd {

class Federator;

ChainListener::ChainListener(
    ChainType chainType,
    ripple::STXChainBridge const sidechain,
    std::optional<ripple::AccountID> submitAccountOpt,
    std::weak_ptr<Federator>&& federator,
    std::optional<ripple::AccountID> signingAccount,
    beast::Journal j)
    : chainType_{chainType}
    , bridge_{sidechain}
    , witnessAccountStr_(
          submitAccountOpt ? ripple::toBase58(*submitAccountOpt)
                           : std::string{})
    , federator_{std::move(federator)}
    , signingAccount_(signingAccount)
    , j_{j}
    , stopHistory_(false)
{
}

// destructor must be defined after WebsocketClient size is known (i.e. it can
// not be defaulted in the header or the unique_ptr declration of
// WebsocketClient won't work)
ChainListener::~ChainListener() = default;

void
ChainListener::init(boost::asio::io_service& ios, beast::IP::Endpoint const& ip)
{
    wsClient_ = std::make_shared<WebsocketClient>(
        [self = shared_from_this()](Json::Value const& msg) {
            self->onMessage(msg);
        },
        [self = shared_from_this()]() { self->onConnect(); },
        ios,
        ip,
        /*headers*/ std::unordered_map<std::string, std::string>{},
        j_);

    wsClient_->connect();
}

void
ChainListener::onConnect()
{
    auto const doorAccStr = ripple::toBase58(bridge_.door(chainType_));
    auto doorAccInfoCb = [self = shared_from_this(),
                          doorAccStr](Json::Value const& msg) {
        self->processAccountInfo(msg);

        Json::Value params;
        params[ripple::jss::account_history_tx_stream] = Json::objectValue;
        params[ripple::jss::account_history_tx_stream][ripple::jss::account] =
            doorAccStr;

        params[ripple::jss::streams] = Json::arrayValue;
        params[ripple::jss::streams].append("ledger");
        if (!self->witnessAccountStr_.empty())
        {
            params[ripple::jss::accounts] = Json::arrayValue;
            params[ripple::jss::accounts].append(self->witnessAccountStr_);
        }
        self->send("subscribe", params);

        self->stopHistory_ = false;
    };

    auto mainFlow = [self = shared_from_this(), doorAccStr, doorAccInfoCb]() {
        Json::Value params;
        params[ripple::jss::account] = doorAccStr;
        params[ripple::jss::signer_lists] = true;

        self->send("account_info", params, doorAccInfoCb);
    };

    auto signAccInfoCb = [self = shared_from_this(),
                          mainFlow](Json::Value const& msg) {
        self->processSigningAccountInfo(msg);
        mainFlow();
    };

    if (signingAccount_)
    {
        Json::Value params;
        params[ripple::jss::account] = ripple::toBase58(*signingAccount_);
        send("account_info", params, signAccInfoCb);
    }
    else
    {
        mainFlow();
    }
}

void
ChainListener::shutdown()
{
    if (wsClient_)
        wsClient_->shutdown();
}

std::uint32_t
ChainListener::send(std::string const& cmd, Json::Value const& params)
{
    return wsClient_->send(cmd, params);
}

void
ChainListener::stopHistoricalTxns()
{
    const auto doorAccStr = ripple::toBase58(bridge_.door(chainType_));

    Json::Value params;
    params[ripple::jss::account_history_tx_stream] = Json::objectValue;
    params[ripple::jss::account_history_tx_stream]
          [ripple::jss::stop_history_tx_only] = true;
    params[ripple::jss::account_history_tx_stream][ripple::jss::account] =
        doorAccStr;
    send("unsubscribe", params);

    stopHistory_ = true;
}

void
ChainListener::send(
    std::string const& cmd,
    Json::Value const& params,
    RpcCallback onResponse)
{
    JLOGV(
        j_.trace(),
        "ChainListener send",
        ripple::jv("command", cmd),
        ripple::jv("params", params),
        ripple::jv("chain_name", to_string(chainType_)));

    auto id = wsClient_->send(cmd, params);
    JLOGV(j_.trace(), "ChainListener send id", ripple::jv("id", id));

    std::lock_guard lock(callbacksMtx_);
    callbacks_.emplace(id, onResponse);
}

template <class E>
void
ChainListener::pushEvent(E&& e)
{
    static_assert(std::is_rvalue_reference_v<decltype(e)>, "");

    if (auto f = federator_.lock())
    {
        f->push(std::move(e));
    }
}

void
ChainListener::onMessage(Json::Value const& msg)
{
    auto callbackOpt = [&]() -> std::optional<RpcCallback> {
        if (msg.isMember(ripple::jss::id) && msg[ripple::jss::id].isIntegral())
        {
            auto callbackId = msg[ripple::jss::id].asUInt();
            std::lock_guard lock(callbacksMtx_);
            auto i = callbacks_.find(callbackId);
            if (i != callbacks_.end())
            {
                auto cb = i->second;
                callbacks_.erase(i);
                return cb;
            }
        }
        return {};
    }();

    if (callbackOpt)
    {
        JLOGV(
            j_.trace(),
            "ChainListener onMessage, reply to a callback",
            ripple::jv("chain_name", to_string(chainType_)),
            ripple::jv("msg", msg.toStyledString()));
        (*callbackOpt)(msg);
    }
    else
    {
        processMessage(msg);
    }
}

namespace {

bool
isDeletedClaimId(Json::Value const& meta, std::uint64_t claimID)
{
    if (!meta.isMember("AffectedNodes"))
        return false;

    for (auto const& an : meta["AffectedNodes"])
    {
        if (!an.isMember("DeletedNode"))
            continue;
        auto const& dn = an["DeletedNode"];
        if (!dn.isMember("FinalFields"))
            continue;
        auto const& ff = dn["FinalFields"];
        auto const optClaimId =
            Json::getOptional<std::uint64_t>(ff, ripple::sfXChainClaimID);

        if (optClaimId == claimID)
            return true;
    }

    return false;
}

bool
isDeletedAccCnt(Json::Value const& meta, std::uint64_t createCnt)
{
    if (!meta.isMember("AffectedNodes"))
        return false;

    for (auto const& an : meta["AffectedNodes"])
    {
        if (!an.isMember("DeletedNode"))
            continue;
        auto const& dn = an["DeletedNode"];
        if (!dn.isMember("FinalFields"))
            continue;
        auto const& ff = dn["FinalFields"];
        auto const optCreateCnt = Json::getOptional<std::uint64_t>(
            ff, ripple::sfXChainAccountCreateCount);

        if (optCreateCnt == createCnt)
            return true;
    }

    return false;
}

}  // namespace

void
ChainListener::processMessage(Json::Value const& msg)
{
    const auto chainName = to_string(chainType_);

    auto ignore_ret = [&](std::string_view reason, auto&&... v) {
        JLOGV(
            j_.trace(),
            "ignoring listener message",
            ripple::jv("chain_name", chainName),
            ripple::jv("reason", reason),
            std::forward<decltype(v)>(v)...);
    };

    auto txnHistoryIndex = [&]() -> std::optional<std::int32_t> {
        // only history stream messages have the index
        if (!msg.isMember(ripple::jss::account_history_tx_index) ||
            !msg[ripple::jss::account_history_tx_index].isIntegral())
            return {};
        // values < 0 are historical txns. values >= 0 are new transactions.
        // Only the initial sync needs historical txns.
        return msg[ripple::jss::account_history_tx_index].asInt();
    }();
    bool const isHistory = txnHistoryIndex && (*txnHistoryIndex < 0);

    if (isHistory && stopHistory_)
        return ignore_ret(
            "stopped processing historical tx",
            ripple::jv(
                ripple::jss::account_history_tx_index.c_str(),
                *txnHistoryIndex));

    // Even though this lock has a large scope, this function does very little
    // processing and should run relatively quickly
    std::lock_guard l{m_};

    JLOGV(
        j_.trace(),
        "chain listener process message",
        ripple::jv("chain_name", chainName),
        ripple::jv("msg", msg.toStyledString()));

    auto tryPushNewLedgerEvent = [&](Json::Value const& result) -> bool {
        if (result.isMember(ripple::jss::fee_base) &&
            result[ripple::jss::fee_base].isIntegral() &&
            result.isMember(ripple::jss::ledger_index) &&
            result[ripple::jss::ledger_index].isIntegral() &&
            result.isMember(ripple::jss::reserve_base) &&
            result.isMember(ripple::jss::reserve_inc) &&
            result.isMember(ripple::jss::fee_ref) &&
            result.isMember(ripple::jss::validated_ledgers))
        {
            event::NewLedger e{
                chainType_,
                result[ripple::jss::ledger_index].asUInt(),
                result[ripple::jss::fee_base].asUInt()};
            pushEvent(std::move(e));
            return true;
        }
        return false;
    };

    if (msg.isMember(ripple::jss::result) &&
        tryPushNewLedgerEvent(msg[ripple::jss::result]))
        return;
    else if (tryPushNewLedgerEvent(msg))
        return;

    bool const history_tx_first =
        msg.isMember(ripple::jss::account_history_tx_first) &&
        msg[ripple::jss::account_history_tx_first].asBool();
    if (history_tx_first)
    {
        pushEvent(event::EndOfHistory{chainType_});
    }

    if (!msg.isMember(ripple::jss::validated) ||
        !msg[ripple::jss::validated].asBool())
        return ignore_ret("not validated");

    if (!msg.isMember(ripple::jss::transaction))
        return ignore_ret("no tx");

    auto const& transaction = msg[ripple::jss::transaction];

    if (!msg.isMember(ripple::jss::meta))
        return ignore_ret("no meta");

    auto const& meta = msg[ripple::jss::meta];

    if (!msg.isMember(ripple::jss::engine_result_code))
        return ignore_ret("no engine result code");

    ripple::TER const txnTER = [&msg] {
        return ripple::TER::fromInt(
            msg[ripple::jss::engine_result_code].asInt());
    }();
    bool const txnSuccess = ripple::isTesSuccess(txnTER);

    auto txnTypeOpt = rpcResultParse::parseXChainTxnType(transaction);
    if (!txnTypeOpt)
        return ignore_ret("not a sidechain transaction");

    auto const txnBridge = rpcResultParse::parseBridge(transaction);
    if (txnBridge && *txnBridge != bridge_)
    {
        // Only keep transactions to or from the door account.
        // Transactions to the account are initiated by users and are are cross
        // chain transactions. Transaction from the account are initiated by
        // federators and need to be monitored for errors. There are two types
        // of transactions that originate from the door account: the second half
        // of a cross chain payment and a refund of a failed cross chain
        // payment.

        // TODO: It is a mistake to filter out based on sidechain.
        // This server should support multiple sidechains
        // Note: the federator stores a hard-coded sidechain in the
        // database, if we remove this filter we need to remove
        // sidechain from the app and listener as well
        return ignore_ret("Sidechain mismatch");
    }

    auto const txnHash = rpcResultParse::parseTxHash(transaction);
    if (!txnHash)
        return ignore_ret("no tx hash");

    auto const txnSeq = rpcResultParse::parseTxSeq(transaction);
    if (!txnSeq)
        return ignore_ret("no txnSeq");

    auto const lgrSeq = rpcResultParse::parseLedgerSeq(msg);
    if (!lgrSeq)
        return ignore_ret("no lgrSeq");

    auto const src = rpcResultParse::parseSrcAccount(transaction);
    if (!src)
        return ignore_ret("no account src");

    auto const dst = rpcResultParse::parseDstAccount(transaction, *txnTypeOpt);

    auto const ledgerBoundary = [&]() -> bool {
        if (msg.isMember(ripple::jss::account_history_boundary) &&
            msg[ripple::jss::account_history_boundary].isBool() &&
            msg[ripple::jss::account_history_boundary].asBool())
        {
            JLOGV(
                j_.trace(),
                "ledger boundary",
                ripple::jv("seq", *lgrSeq),
                ripple::jv("chain_name", chainName));
            return true;
        }
        return false;
    }();

    std::optional<ripple::STAmount> deliveredAmt =
        rpcResultParse::parseDeliveredAmt(transaction, meta);

    auto const [chainDir, oppositeChainDir] =
        [&]() -> std::pair<ChainDir, ChainDir> {
        using enum ChainDir;
        if (chainType_ == ChainType::locking)
            return {issuingToLocking, lockingToIssuing};
        return {lockingToIssuing, issuingToLocking};
    }();

    switch (*txnTypeOpt)
    {
        case XChainTxnType::xChainClaim: {
            auto const claimID = Json::getOptional<std::uint64_t>(
                transaction, ripple::sfXChainClaimID);

            if (!claimID)
                return ignore_ret("no claimID");
            if (!dst)
                return ignore_ret("no dst in xchain claim");

            using namespace event;
            XChainTransferResult e{
                chainDir,
                *dst,
                deliveredAmt,
                *claimID,
                *lgrSeq,
                *txnHash,
                txnTER,
                txnHistoryIndex};
            pushEvent(std::move(e));
        }
        break;
        case XChainTxnType::xChainCommit: {
            auto const claimID = Json::getOptional<std::uint64_t>(
                transaction, ripple::sfXChainClaimID);

            if (!claimID)
                return ignore_ret("no claimID");
            if (!txnBridge)
                return ignore_ret("no bridge in xchain commit");

            using namespace event;
            XChainCommitDetected e{
                oppositeChainDir,
                *src,
                *txnBridge,
                deliveredAmt,
                *claimID,
                dst,
                *lgrSeq,
                *txnHash,
                txnTER,
                txnHistoryIndex,
                ledgerBoundary};
            pushEvent(std::move(e));
        }
        break;
        case XChainTxnType::xChainAccountCreateCommit: {
            auto const createCount = rpcResultParse::parseCreateCount(meta);
            if (!createCount)
                return ignore_ret("no createCount");
            if (!txnBridge)
                return ignore_ret("no bridge in xchain commit");

            auto const rewardAmt = rpcResultParse::parseRewardAmt(transaction);
            if (!rewardAmt)
                return ignore_ret("no reward amt in xchain create account");

            if (!dst)
                return ignore_ret("no dst in xchain create account");

            using namespace event;
            XChainAccountCreateCommitDetected e{
                oppositeChainDir,
                *src,
                *txnBridge,
                deliveredAmt,
                *rewardAmt,
                *createCount,
                *dst,
                *lgrSeq,
                *txnHash,
                txnTER,
                txnHistoryIndex,
                ledgerBoundary};
            pushEvent(std::move(e));
        }
        break;
        case XChainTxnType::xChainCreateBridge: {
            if (!txnBridge)
                return ignore_ret("no bridge in xChainCreateBridge");

            if (!history_tx_first && isHistory)
                pushEvent(event::EndOfHistory{chainType_});
        }
        break;
#ifdef USE_BATCH_ATTESTATION
        case XChainTxnType::xChainAddAttestationBatch: {
            if (rpcResultParse::fieldMatchesStr(
                    transaction,
                    ripple::jss::Account,
                    witnessAccountStr_.c_str()) &&
                txnSeq)
            {
                pushEvent(
                    event::XChainAttestsResult{chainType_, *txnSeq, txnTER});
                return;
            }
            else
                return ignore_ret("not an attestation sent from this server");
        }
        break;
#endif
        case XChainTxnType::xChainAddAccountCreateAttestation:
        case XChainTxnType::xChainAddClaimAttestation: {
            bool const isOwn = rpcResultParse::fieldMatchesStr(
                transaction, ripple::jss::Account, witnessAccountStr_.c_str());
            if ((isHistory || isOwn) && txnSeq)
            {
                JLOGV(
                    j_.trace(),
                    "Attestation processing",
                    ripple::jv("chain_name", chainName),
                    ripple::jv("src", *src),
                    ripple::jv(
                        "dst",
                        !dst || !*dst ? std::string() : ripple::toBase58(*dst)),
                    ripple::jv("witnessAccountStr_", witnessAccountStr_));

                auto osrc = rpcResultParse::parseOtherSrcAccount(
                    transaction, *txnTypeOpt);
                auto odst = rpcResultParse::parseOtherDstAccount(
                    transaction, *txnTypeOpt);
                if (!osrc ||
                    ((txnTypeOpt ==
                      XChainTxnType::xChainAddAccountCreateAttestation) &&
                     !odst))
                    return ignore_ret(
                        "osrc/odst account missing",
                        ripple::jv("witnessAccountStr_", witnessAccountStr_));

                std::optional<std::uint64_t> claimID, accountCreateCount;

                if (txnTypeOpt == XChainTxnType::xChainAddClaimAttestation)
                {
                    claimID = Json::getOptional<std::uint64_t>(
                        transaction, ripple::sfXChainClaimID);
                    if (!claimID)
                        return ignore_ret("no claimID");

                    if (!isOwn && !isDeletedClaimId(meta, *claimID))
                        return ignore_ret("claimID not in DeletedNode");
                }

                if (txnTypeOpt ==
                    XChainTxnType::xChainAddAccountCreateAttestation)
                {
                    accountCreateCount = Json::getOptional<std::uint64_t>(
                        transaction, ripple::sfXChainAccountCreateCount);
                    if (!accountCreateCount)
                        return ignore_ret("no accountCreateCount");

                    if (!isOwn && !isDeletedAccCnt(meta, *accountCreateCount))
                        return ignore_ret(
                            "accountCreateCount not in DeletedNode");
                }

                pushEvent(event::XChainAttestsResult{
                    chainType_,
                    *txnSeq,
                    *txnHash,
                    txnTER,
                    isHistory,
                    *txnTypeOpt,
                    *osrc,
                    odst ? *odst : ripple::AccountID(),
                    accountCreateCount,
                    claimID});
                return;
            }
            else
                return ignore_ret(
                    "not an attestation sent from this server",
                    ripple::jv("witnessAccountStr_", witnessAccountStr_));
        }
        break;
        case XChainTxnType::SignerListSet: {
            if (txnSuccess && !isHistory)
                processSignerListSet(transaction);
            return;
        }
        break;
        case XChainTxnType::AccountSet: {
            if (txnSuccess && !isHistory)
                processAccountSet(transaction);
            return;
        }
        break;
        case XChainTxnType::SetRegularKey: {
            if (txnSuccess && !isHistory)
                processSetRegularKey(transaction);
            return;
        }
        break;
    }
}

namespace {
std::optional<std::unordered_set<ripple::AccountID>>
processSignerListSetGeneral(
    Json::Value const& msg,
    std::string_view const chainName,
    std::string_view const errTopic,
    beast::Journal j)
{
    auto warn_ret = [&](std::string_view reason) {
        JLOGV(
            j.warn(),
            errTopic,
            ripple::jv("reason", reason),
            ripple::jv("chain_name", chainName),
            ripple::jv("msg", msg));
        return std::optional<std::unordered_set<ripple::AccountID>>();
    };

    if (msg.isMember("SignerQuorum"))
    {
        unsigned const signerQuorum = msg["SignerQuorum"].asUInt();
        if (!signerQuorum)
            return warn_ret("'SignerQuorum' is null");
    }
    else
        return warn_ret("'SignerQuorum' missed");

    if (!msg.isMember("SignerEntries"))
        return warn_ret("'SignerEntries' missed");
    auto const& signerEntries = msg["SignerEntries"];
    if (!signerEntries.isArray())
        return warn_ret("'SignerEntries' is not an array");

    std::unordered_set<ripple::AccountID> entries;
    for (auto const& superEntry : signerEntries)
    {
        if (!superEntry.isMember("SignerEntry"))
            return warn_ret("'SignerEntry' missed");

        auto const& entry = superEntry["SignerEntry"];
        if (!entry.isMember(ripple::jss::Account))
            return warn_ret("'Account' missed");

        auto const& jAcc = entry[ripple::jss::Account];
        auto parsed = ripple::parseBase58<ripple::AccountID>(jAcc.asString());
        if (!parsed)
            return warn_ret("invalid 'Account'");

        entries.insert(parsed.value());
    }

    return {std::move(entries)};
}

}  // namespace

void
ChainListener::processAccountInfo(Json::Value const& msg) noexcept
{
    std::string const chainName = to_string(chainType_);
    std::string_view const errTopic = "ignoring account_info message";

    auto warn_ret = [&, this](std::string_view reason) {
        JLOGV(
            j_.warn(),
            errTopic,
            ripple::jv("reason", reason),
            ripple::jv("chain_name", chainName),
            ripple::jv("msg", msg));
    };

    try
    {
        if (!msg.isMember(ripple::jss::result))
            return warn_ret("'result' missed");

        auto const& jres = msg[ripple::jss::result];
        if (!jres.isMember(ripple::jss::account_data))
            return warn_ret("'account_data' missed");

        auto const& jaccData = jres[ripple::jss::account_data];
        if (!jaccData.isMember(ripple::jss::Account))
            return warn_ret("'Account' missed");

        auto const& jAcc = jaccData[ripple::jss::Account];
        auto const parsedAcc =
            ripple::parseBase58<ripple::AccountID>(jAcc.asString());
        if (!parsedAcc)
            return warn_ret("invalid 'Account'");

        // check disable master key
        {
            auto const fDisableMaster = jaccData.isMember(ripple::jss::Flags)
                ? static_cast<bool>(
                      jaccData[ripple::jss::Flags].asUInt() &
                      ripple::lsfDisableMaster)
                : false;

            pushEvent(event::XChainAccountSet{
                chainType_, *parsedAcc, fDisableMaster});
        }

        // check regular key
        {
            if (jaccData.isMember("RegularKey"))
            {
                std::string const regularKeyStr =
                    jaccData["RegularKey"].asString();
                auto opRegularDoorId =
                    ripple::parseBase58<ripple::AccountID>(regularKeyStr);

                pushEvent(event::XChainSetRegularKey{
                    chainType_,
                    *parsedAcc,
                    opRegularDoorId ? std::move(*opRegularDoorId)
                                    : ripple::AccountID()});
            }
        }

        // check signer list
        {
            if (!jaccData.isMember(ripple::jss::signer_lists))
                return warn_ret("'signer_lists' missed");
            auto const& jslArray = jaccData[ripple::jss::signer_lists];
            if (!jslArray.isArray() || jslArray.size() != 1)
                return warn_ret("'signer_lists'  isn't array of size 1");

            auto opEntries = processSignerListSetGeneral(
                jslArray[0u], chainName, errTopic, j_);
            if (!opEntries)
                return;

            pushEvent(event::XChainSignerListSet{
                chainType_, *parsedAcc, std::move(*opEntries)});
        }
    }
    catch (std::exception const& e)
    {
        JLOGV(
            j_.warn(),
            errTopic,
            ripple::jv("exception", e.what()),
            ripple::jv("chain_name", chainName),
            ripple::jv("msg", msg));
    }
    catch (...)
    {
        JLOGV(
            j_.warn(),
            errTopic,
            ripple::jv("exception", "unknown exception"),
            ripple::jv("chain_name", chainName),
            ripple::jv("msg", msg));
    }
}

void
ChainListener::processSigningAccountInfo(Json::Value const& msg) noexcept
{
    std::string const chainName = to_string(chainType_);
    std::string_view const errTopic = "ignoring signing account_info message";

    auto warn_ret = [&, this](std::string_view reason) {
        JLOGV(
            j_.warn(),
            errTopic,
            ripple::jv("reason", reason),
            ripple::jv("chain_name", chainName),
            ripple::jv("msg", msg));
    };

    try
    {
        if (!msg.isMember(ripple::jss::result))
            return warn_ret("'result' missed");

        auto const& jres = msg[ripple::jss::result];
        if (!jres.isMember(ripple::jss::account_data))
            return warn_ret("'account_data' missed");

        auto const& jaccData = jres[ripple::jss::account_data];
        if (!jaccData.isMember(ripple::jss::Account))
            return warn_ret("'Account' missed");

        auto const& jAcc = jaccData[ripple::jss::Account];
        auto const parsedAcc =
            ripple::parseBase58<ripple::AccountID>(jAcc.asString());
        if (!parsedAcc)
            return warn_ret("invalid 'Account'");

        bool const fDisableMaster = jaccData.isMember(ripple::jss::Flags)
            ? static_cast<bool>(
                  jaccData[ripple::jss::Flags].asUInt() &
                  ripple::lsfDisableMaster)
            : false;

        std::optional<ripple::AccountID> regularAcc;
        if (jaccData.isMember("RegularKey"))
        {
            std::string const regularKeyStr = jaccData["RegularKey"].asString();
            regularAcc = ripple::parseBase58<ripple::AccountID>(regularKeyStr);
        }

        auto f = federator_.lock();
        if (!f)
            return warn_ret("federator not available");

        f->checkSigningKey(chainType_, fDisableMaster, regularAcc);
    }
    catch (std::exception const& e)
    {
        JLOGV(
            j_.warn(),
            errTopic,
            ripple::jv("exception", e.what()),
            ripple::jv("chain_name", chainName),
            ripple::jv("msg", msg));
    }
    catch (...)
    {
        JLOGV(
            j_.warn(),
            errTopic,
            ripple::jv("exception", "unknown exception"),
            ripple::jv("chain_name", chainName),
            ripple::jv("msg", msg));
    }
}

void
ChainListener::processSignerListSet(Json::Value const& msg) noexcept
{
    std::string const chainName = to_string(chainType_);
    std::string_view const errTopic = "ignoring SignerListSet message";

    auto warn_ret = [&, this](std::string_view reason) {
        JLOGV(
            j_.warn(),
            errTopic,
            ripple::jv("reason", reason),
            ripple::jv("chain_name", chainName),
            ripple::jv("msg", msg));
    };

    try
    {
        auto const lockingDoorStr =
            ripple::toBase58(bridge_.lockingChainDoor());
        auto const issuingDoorStr =
            ripple::toBase58(bridge_.issuingChainDoor());

        if (!msg.isMember(ripple::jss::Account))
            return warn_ret("'Account' missed");

        auto const txAccStr = msg[ripple::jss::Account].asString();
        if ((txAccStr != lockingDoorStr) && (txAccStr != issuingDoorStr))
            return warn_ret("unknown tx account");

        auto const parsedAcc = ripple::parseBase58<ripple::AccountID>(txAccStr);
        if (!parsedAcc)
            return warn_ret("invalid 'Account'");

        auto opEntries =
            processSignerListSetGeneral(msg, chainName, errTopic, j_);
        if (!opEntries)
            return;

        event::XChainSignerListSet evSignSet{
            .chainType_ = txAccStr == lockingDoorStr ? ChainType::locking
                                                     : ChainType::issuing,
            .masterDoorID_ = *parsedAcc,
            .signerList_ = std::move(*opEntries)};
        if (evSignSet.chainType_ != chainType_)
        {
            // This is strange but it is processed well by rippled
            // so we can proceed
            JLOGV(
                j_.warn(),
                "processing signer list message",
                ripple::jv("warning", "Door account type mismatch"),
                ripple::jv("chain_type", to_string(chainType_)),
                ripple::jv("tx_type", to_string(evSignSet.chainType_)));
        }

        pushEvent(std::move(evSignSet));
    }
    catch (std::exception const& e)
    {
        JLOGV(
            j_.warn(),
            errTopic,
            ripple::jv("exception", e.what()),
            ripple::jv("chain_name", chainName),
            ripple::jv("msg", msg));
    }
    catch (...)
    {
        JLOGV(
            j_.warn(),
            errTopic,
            ripple::jv("exception", "unknown exception"),
            ripple::jv("chain_name", chainName),
            ripple::jv("msg", msg));
    }
}

void
ChainListener::processAccountSet(Json::Value const& msg) noexcept
{
    std::string const chainName = to_string(chainType_);
    std::string_view const errTopic = "ignoring AccountSet message";

    auto warn_ret = [&, this](std::string_view reason) {
        JLOGV(
            j_.warn(),
            errTopic,
            ripple::jv("reason", reason),
            ripple::jv("chain_name", chainName),
            ripple::jv("msg", msg));
    };

    try
    {
        auto const lockingDoorStr =
            ripple::toBase58(bridge_.lockingChainDoor());
        auto const issuingDoorStr =
            ripple::toBase58(bridge_.issuingChainDoor());

        if (!msg.isMember(ripple::jss::Account))
            return warn_ret("'Account' missed");

        auto const txAccStr = msg[ripple::jss::Account].asString();
        if ((txAccStr != lockingDoorStr) && (txAccStr != issuingDoorStr))
            return warn_ret("unknown tx account");

        auto const parsedAcc = ripple::parseBase58<ripple::AccountID>(txAccStr);
        if (!parsedAcc)
            return warn_ret("invalid 'Account'");

        if (!msg.isMember(ripple::jss::SetFlag) &&
            !msg.isMember(ripple::jss::ClearFlag))
            return warn_ret("'XXXFlag' missed");

        bool const setFlag = msg.isMember(ripple::jss::SetFlag);
        unsigned const flag = setFlag ? msg[ripple::jss::SetFlag].asUInt()
                                      : msg[ripple::jss::ClearFlag].asUInt();
        if (flag != ripple::asfDisableMaster)
            return warn_ret("not 'asfDisableMaster' flag");

        event::XChainAccountSet evAccSet{chainType_, *parsedAcc, setFlag};
        if (evAccSet.chainType_ != chainType_)
        {
            // This is strange but it is processed well by rippled
            // so we can proceed
            JLOGV(
                j_.warn(),
                "processing account set",
                ripple::jv("warning", "Door account type mismatch"),
                ripple::jv("chain_type", to_string(chainType_)),
                ripple::jv("tx_type", to_string(evAccSet.chainType_)));
        }
        pushEvent(std::move(evAccSet));
    }
    catch (std::exception const& e)
    {
        JLOGV(
            j_.warn(),
            errTopic,
            ripple::jv("exception", e.what()),
            ripple::jv("chain_name", chainName),
            ripple::jv("msg", msg));
    }
    catch (...)
    {
        JLOGV(
            j_.warn(),
            errTopic,
            ripple::jv("exception", "unknown exception"),
            ripple::jv("chain_name", chainName),
            ripple::jv("msg", msg));
    }
}

void
ChainListener::processSetRegularKey(Json::Value const& msg) noexcept
{
    std::string const chainName = to_string(chainType_);
    std::string_view const errTopic = "ignoring SetRegularKey message";

    auto warn_ret = [&, this](std::string_view reason) {
        JLOGV(
            j_.warn(),
            errTopic,
            ripple::jv("reason", reason),
            ripple::jv("chain_name", chainName),
            ripple::jv("msg", msg));
    };

    try
    {
        auto const lockingDoorStr =
            ripple::toBase58(bridge_.lockingChainDoor());
        auto const issuingDoorStr =
            ripple::toBase58(bridge_.issuingChainDoor());

        if (!msg.isMember(ripple::jss::Account))
            return warn_ret("'Account' missed");

        auto const txAccStr = msg[ripple::jss::Account].asString();
        if ((txAccStr != lockingDoorStr) && (txAccStr != issuingDoorStr))
            return warn_ret("unknown tx account");

        auto const parsedAcc = ripple::parseBase58<ripple::AccountID>(txAccStr);
        if (!parsedAcc)
            return warn_ret("invalid 'Account'");

        event::XChainSetRegularKey evSetKey{
            .chainType_ = txAccStr == lockingDoorStr ? ChainType::locking
                                                     : ChainType::issuing,
            .masterDoorID_ = *parsedAcc};

        if (evSetKey.chainType_ != chainType_)
        {
            // This is strange but it is processed well by rippled
            // so we can proceed
            JLOGV(
                j_.warn(),
                "processing account set",
                ripple::jv("warning", "Door account type mismatch"),
                ripple::jv("chain_type", to_string(chainType_)),
                ripple::jv("tx_type", to_string(evSetKey.chainType_)));
        }

        std::string const regularKeyStr = msg.isMember("RegularKey")
            ? msg["RegularKey"].asString()
            : std::string();
        if (!regularKeyStr.empty())
        {
            auto opRegularDoorId =
                ripple::parseBase58<ripple::AccountID>(regularKeyStr);
            if (!opRegularDoorId)
                return warn_ret("invalid 'RegularKey'");
            evSetKey.regularDoorID_ = std::move(*opRegularDoorId);
        }

        pushEvent(std::move(evSetKey));
    }
    catch (std::exception const& e)
    {
        JLOGV(
            j_.warn(),
            errTopic,
            ripple::jv("exception", e.what()),
            ripple::jv("chain_name", chainName),
            ripple::jv("msg", msg));
    }
    catch (...)
    {
        JLOGV(
            j_.warn(),
            errTopic,
            ripple::jv("exception", "unknown exception"),
            ripple::jv("chain_name", chainName),
            ripple::jv("msg", msg));
    }
}

void
ChainListener::processTx(Json::Value const& v) noexcept
{
    std::string const chainName = to_string(chainType_);
    std::string_view const errTopic = "ignoring tx RPC response";

    auto warn_ret = [&, this](std::string_view reason) {
        JLOGV(
            j_.warn(),
            errTopic,
            ripple::jv("reason", reason),
            ripple::jv("chain_name", chainName),
            ripple::jv("msg", v));
    };

    try
    {
        if (!v.isMember(ripple::jss::result))
            return warn_ret("missing result field");

        auto const& msg = v[ripple::jss::result];

        if (!msg.isMember(ripple::jss::validated) ||
            !msg[ripple::jss::validated].asBool())
            return warn_ret("not validated");

        if (!msg.isMember(ripple::jss::meta))
            return warn_ret("missing meta field");

        auto const& meta = msg[ripple::jss::meta];

        if (!(meta.isMember("TransactionResult") &&
              meta["TransactionResult"].isString() &&
              meta["TransactionResult"].asString() == "tesSUCCESS"))
            return warn_ret("missing or bad TransactionResult");

        auto txnTypeOpt = rpcResultParse::parseXChainTxnType(msg);
        if (!txnTypeOpt)
            return warn_ret("missing or bad tx type");

        auto const txnHash = rpcResultParse::parseTxHash(msg);
        if (!txnHash)
            return warn_ret("missing or bad tx hash");

        auto const txnBridge = rpcResultParse::parseBridge(msg);
        if (!txnBridge)  // TODO check bridge match
            return warn_ret("missing or bad bridge");

        auto const txnSeq = rpcResultParse::parseTxSeq(msg);
        if (!txnSeq)
            return warn_ret("missing or bad tx sequence");

        auto const lgrSeq = rpcResultParse::parseLedgerSeq(msg);
        if (!lgrSeq)
            return warn_ret("missing or bad ledger sequence");

        auto const src = rpcResultParse::parseSrcAccount(msg);
        if (!src)
            return warn_ret("missing or bad source account");

        auto const dst = rpcResultParse::parseDstAccount(msg, *txnTypeOpt);

        std::optional<ripple::STAmount> deliveredAmt =
            rpcResultParse::parseDeliveredAmt(msg, meta);

        auto const oppositeChainDir = chainType_ == ChainType::locking
            ? ChainDir::lockingToIssuing
            : ChainDir::issuingToLocking;

        switch (*txnTypeOpt)
        {
            case XChainTxnType::xChainCommit: {
                auto const claimID = Json::getOptional<std::uint64_t>(
                    msg, ripple::sfXChainClaimID);
                if (!claimID)
                    return warn_ret("no claimID");

                using namespace event;
                XChainCommitDetected e{
                    oppositeChainDir,
                    *src,
                    *txnBridge,
                    deliveredAmt,
                    *claimID,
                    dst,
                    *lgrSeq,
                    *txnHash,
                    ripple::tesSUCCESS,
                    {},
                    false};
                pushEvent(std::move(e));
            }
            break;
            case XChainTxnType::xChainAccountCreateCommit: {
                auto const createCount = rpcResultParse::parseCreateCount(meta);
                if (!createCount)
                    return warn_ret("missing or bad createCount");

                auto const rewardAmt = rpcResultParse::parseRewardAmt(msg);
                if (!rewardAmt)
                    return warn_ret("missing or bad rewardAmount");

                if (!dst)
                    return warn_ret("missing or bad destination account");

                using namespace event;
                XChainAccountCreateCommitDetected e{
                    oppositeChainDir,
                    *src,
                    *txnBridge,
                    deliveredAmt,
                    *rewardAmt,
                    *createCount,
                    *dst,
                    *lgrSeq,
                    *txnHash,
                    ripple::tesSUCCESS,
                    {},
                    false};
                pushEvent(std::move(e));
            }
            break;
            default:
                return warn_ret("wrong transaction type");
        }
    }
    catch (std::exception const& e)
    {
        JLOGV(
            j_.warn(),
            errTopic,
            ripple::jv("exception", e.what()),
            ripple::jv("chain_name", chainName),
            ripple::jv("msg", v));
    }
    catch (...)
    {
        JLOGV(
            j_.warn(),
            errTopic,
            ripple::jv("exception", "unknown exception"),
            ripple::jv("chain_name", chainName),
            ripple::jv("msg", v));
    }
}

Json::Value
ChainListener::getInfo() const
{
    std::lock_guard l{m_};

    // TODO
    Json::Value ret{Json::objectValue};
    return ret;
}

}  // namespace xbwd
