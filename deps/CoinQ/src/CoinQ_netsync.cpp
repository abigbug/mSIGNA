///////////////////////////////////////////////////////////////////////////////
//
// CoinQ_netsync.cpp
//
// Copyright (c) 2013 Eric Lombrozo
//
// All Rights Reserved.

#include "CoinQ_netsync.h"

#include "CoinQ_typedefs.h"

#include <CoinCore/MerkleTree.h>

#include <stdint.h>

#include <logger/logger.h>

using namespace CoinQ::Network;
using namespace std;

NetworkSync::NetworkSync(const CoinQ::CoinParams& coinParams) :
    m_coinParams(coinParams),
    m_bStarted(false),
    m_ioServiceThread(nullptr),
    m_work(m_ioService),
    m_peer(m_ioService),
    m_bConnected(false),
    m_bFetchingHeaders(false),
    m_bHeadersSynched(false),
    m_bFetchingBlocks(false),
    m_bBlocksSynched(false)
{
    // Select hash functions
    Coin::CoinBlockHeader::setHashFunc(m_coinParams.block_header_hash_function());
    Coin::CoinBlockHeader::setPOWHashFunc(m_coinParams.block_header_pow_hash_function());

    // Start the service thread
    m_ioServiceThread = new boost::thread(boost::bind(&CoinQ::io_service_t::run, &m_ioService));
/*
    // Subscribe block tree handlers 
    m_blockTree.subscribeRemoveBestChain([&](const ChainHeader& header)
    {
        notifyBlockTreeChanged();
        notifyRemoveBestChain(header);
    });

    m_blockTree.subscribeAddBestChain([&](const ChainHeader& header)
    {
        notifyBlockTreeChanged();
        notifyAddBestChain(header);
        uchar_vector hash = header.getHashLittleEndian();
        std::stringstream status;
        status << "Added to best chain: " << hash.getHex() << " Height: " << header.height << " ChainWork: " << header.chainWork.getDec();
        notifyStatus(status.str());
    });
*/

    // Subscribe peer handlers
    m_peer.subscribeOpen([&](CoinQ::Peer& /*peer*/)
    {
        m_bConnected = true;
        notifyOpen();
        try
        {
            if (m_bloomFilter.isSet())
            {
                Coin::FilterLoadMessage filterLoad(m_bloomFilter.getNHashFuncs(), m_bloomFilter.getNTweak(), m_bloomFilter.getNFlags(), m_bloomFilter.getFilter());
                m_peer.send(filterLoad);
                LOGGER(trace) << "Sent filter to peer." << std::endl;
            }

//            notifyStatus("Fetching block headers...");
            m_peer.getHeaders(m_blockTree.getLocatorHashes(-1));
        }
        catch (const std::exception& e)
        {
            LOGGER(error) << "NetworkSync - m_peer open handler - " << e.what() << std::endl;
            notifyBlockTreeError(e.what());
        }
    });

    m_peer.subscribeClose([&](CoinQ::Peer& /*peer*/)
    {
        stop();
        notifyClose();
    });

    m_peer.subscribeTimeout([&](CoinQ::Peer& /*peer*/)
    {
        notifyTimeout();
    });

    m_peer.subscribeConnectionError([&](CoinQ::Peer& /*peer*/, const std::string& error)
    {
        notifyConnectionError(error);
    });

    m_peer.subscribeProtocolError([&](CoinQ::Peer& /*peer*/, const std::string& error)
    {
        notifyProtocolError(error);
    });

    m_peer.subscribeInv([&](CoinQ::Peer& peer, const Coin::Inventory& inv)
    {
        if (!m_bConnected) return;
        LOGGER(trace) << "Received inventory message:" << std::endl << inv.toIndentedString() << std::endl;

        using namespace Coin;
        GetDataMessage getData;
        for (auto& item: inv.items)
        {
            switch (item.itemType)
            {
            case MSG_TX:
                if (m_bBlocksSynched)
                {
                    getData.items.push_back(item);
                }
                break;
            case MSG_BLOCK:
                if (m_bHeadersSynched)
                {
                    getData.items.push_back(InventoryItem(MSG_FILTERED_BLOCK, item.hash));
                }
                break;
            default:
                break;
            } 
        }

        if (!getData.items.empty()) { m_peer.send(getData); }
    });

    m_peer.subscribeTx([&](CoinQ::Peer& /*peer*/, const Coin::Transaction& tx)
    {
        if (!m_bConnected) return;
        bytes_t txhash = tx.getHashLittleEndian();
        LOGGER(trace) << "Received transaction: " << uchar_vector(txhash).getHex() << std::endl;

        if (m_bBlocksSynched)
        {
            notifyNewTx(tx);
            return;
        }

        try
        {
            if (m_bFetchingBlocks)
            {
                if (m_currentMerkleTxHashes.empty()) throw runtime_error("Should not be receiving transactions before blocks when fetching blocks.");

                if (m_currentMerkleTxHashes.front() != txhash) throw runtime_error("Transaction received out of order.");
                m_currentMerkleTxHashes.pop();
                notifyMerkleTx(m_currentMerkleBlock, tx, m_currentMerkleTxIndex++, m_currentMerkleTxCount);
                if (m_bBlocksFetched && m_currentMerkleTxIndex == m_currentMerkleTxCount)
                {
                    m_bBlocksSynched = true;
                    notifyBlocksSynched();
                }
            }
            else
            {
                throw runtime_error("Should not be receiving transactions if not synched and not fetching blocks.");
            }
        }
        catch (const exception& e)
        {
            notifyProtocolError(e.what());

            // TODO: Attempt to recover
            
        }
    });

    m_peer.subscribeHeaders([&](CoinQ::Peer& peer, const Coin::HeadersMessage& headersMessage)
    {
        if (!m_bConnected) return;
        LOGGER(trace) << "Received headers message..." << std::endl;

        try
        {
            if (headersMessage.headers.size() > 0)
            {
                boost::lock_guard<boost::mutex> syncLock(m_syncMutex);
                m_bFetchingHeaders = true;
                notifyFetchingHeaders();
                for (auto& item: headersMessage.headers)
                {
                    try
                    {
                        if (m_blockTree.insertHeader(item))
                        {
                            m_bHeadersSynched = false;
                        }
                    }
                    catch (const std::exception& e)
                    {
                        std::stringstream err;
                        err << "Block tree insertion error for block " << item.getHashLittleEndian().getHex() << ": " << e.what(); // TODO: localization
                        LOGGER(error) << err.str() << std::endl;
                        notifyBlockTreeError(err.str());
                        throw e;
                    }
                }

                LOGGER(trace)   << "Processed " << headersMessage.headers.size() << " headers."
                                << " mBestHeight: " << m_blockTree.getBestHeight()
                                << " mTotalWork: " << m_blockTree.getTotalWork().getDec()
                                << " Attempting to fetch more headers..." << std::endl;

                notifyBlockTreeChanged();
                std::stringstream status;
                status << "Best Height: " << m_blockTree.getBestHeight() << " / " << "Total Work: " << m_blockTree.getTotalWork().getDec();
                notifyStatus(status.str());

                peer.getHeaders(m_blockTree.getLocatorHashes(1));
            }
            else
            {  
                notifyStatus("Flushing block chain to file...");

                {
                    m_blockTree.flushToFile(m_blockTreeFile);
                    m_bHeadersSynched = true;
                }

                notifyStatus("Done flushing block chain to file");
                notifyHeadersSynched();
            }
        }
        catch (const std::exception& e)
        {
            LOGGER(error) << "block tree exception: " << e.what() << std::endl;
            m_bFetchingHeaders = false;
        }
    });

    m_peer.subscribeBlock([&](CoinQ::Peer& /*peer*/, const Coin::CoinBlock& block)
    {
        if (!m_bConnected) return;
        uchar_vector hash = block.blockHeader.getHashLittleEndian();
        boost::lock_guard<boost::mutex> lock(m_syncMutex);
        try {
            if (m_blockTree.hasHeader(hash))
            {
                notifyBlock(block);
            }
            else if (m_blockTree.insertHeader(block.blockHeader))
            {
                notifyStatus("Flushing block chain to file...");
                m_blockTree.flushToFile(m_blockTreeFile);
                m_bHeadersSynched = true;
                notifyStatus("Done flushing block chain to file");
                notifyHeadersSynched();
                notifyBlock(block);
            }
            else
            {
                LOGGER(debug) << "NetworkSync block handler - block rejected - hash: " << hash.getHex() << std::endl;
            }
        }
        catch (const std::exception& e)
        {
            LOGGER(error) << "NetworkSync block handler - block hash: " << hash.getHex() << " - " << e.what() << std::endl;
            notifyStatus("NetworkSync block handler error.");
        }
    });

    m_peer.subscribeMerkleBlock([&](CoinQ::Peer& /*peer*/, const Coin::MerkleBlock& merkleBlock)
    {
        if (!m_bConnected) return;
        LOGGER(trace) << "Received merkle block:" << std::endl << merkleBlock.toIndentedString() << std::endl;
        uchar_vector hash = merkleBlock.blockHeader.getHashLittleEndian();

        try {
            if (!m_currentMerkleTxHashes.empty()) throw std::runtime_error("Block was received before getting transactions from last block.");

            if (m_blockTree.hasHeader(hash)) {
                // Do nothing but skip over last else.
            }
            else if (m_blockTree.insertHeader(merkleBlock.blockHeader)) {
                notifyStatus("Flushing block chain to file...");
                m_blockTree.flushToFile(m_blockTreeFile);
                notifyStatus("Done flushing block chain to file");
                m_bHeadersSynched = true;
                m_bBlocksFetched = false;
                m_bBlocksSynched = false;
                notifyHeadersSynched();
            }
            else {
                m_bHeadersSynched = false;
                m_bBlocksSynched = false;
                m_bBlocksFetched = false;
                m_bFetchingBlocks = false;

                // Possible reorg
                LOGGER(error) << "NetworkSync merkle block handler - block rejected: " << hash.getHex() << " - possible reorg." << endl;
                try
                {
                    m_peer.getHeaders(m_blockTree.getLocatorHashes(-1));
                }
                catch (const exception& e) {
                    LOGGER(error) << "Block tree error: " << e.what() << endl;
                    notifyBlockTreeError(e.what());
                }
                return;
            }

            if (m_bFetchingBlocks)
            {
                // Set tx hashes
                Coin::PartialMerkleTree tree(merkleBlock.nTxs, merkleBlock.hashes, merkleBlock.flags, merkleBlock.blockHeader.merkleRoot);
                ChainHeader header = m_blockTree.getHeader(hash);
                m_currentMerkleBlock = ChainMerkleBlock(merkleBlock, true, header.height, header.chainWork);
                std::vector<uchar_vector> txhashes = tree.getTxHashesLittleEndianVector();
                for (auto& txhash: txhashes) { m_currentMerkleTxHashes.push(txhash); }
                m_currentMerkleTxIndex = 0;
                m_currentMerkleTxCount = txhashes.size();

                notifyMerkleBlock(m_currentMerkleBlock);

                uint32_t bestHeight = m_blockTree.getBestHeight();
                if (bestHeight > (uint32_t)header.height) // We still need to fetch more blocks
                {
                    const ChainHeader& nextHeader = m_blockTree.getHeader(header.height + 1);
                    uchar_vector hash = nextHeader.getHashLittleEndian();
                    std::string hashString = hash.getHex();

                    std::stringstream status;
                    status << "Asking for block " << hashString << " / height: " << nextHeader.height;
                    LOGGER(debug) << status.str() << std::endl;
                    notifyStatus(status.str());
                    m_lastRequestedBlockHeight = nextHeader.height;
                    try
                    {
                        m_peer.getFilteredBlock(hash);
                    }
                    catch (const exception& e)
                    {
                        notifyConnectionError(e.what());
                    }
                }
                else if (bestHeight == m_lastRequestedBlockHeight && bestHeight == (uint32_t)header.height)
                {
                    m_bBlocksFetched = true;
                    if (m_currentMerkleTxHashes.empty())
                    {
                        m_bBlocksSynched = true;
                        notifyBlocksSynched();
                    }
                }
            }
        }
        catch (const exception& e) {
            LOGGER(error) << "NetworkSync - protocol error: " << e.what() << std::endl;
            notifyProtocolError(e.what());
        }
    });
}

NetworkSync::~NetworkSync()
{
    stop();
    m_ioService.stop();
    m_ioServiceThread->join();
    delete m_ioServiceThread;
}

void NetworkSync::setCoinParams(const CoinQ::CoinParams& coinParams)
{
    if (m_bStarted) throw std::runtime_error("NetworkSync::setCoinParams() - must be stopped to set coin parameters.");
    boost::lock_guard<boost::mutex> lock(m_startMutex);
    if (m_bStarted) throw std::runtime_error("NetworkSync::setCoinParams() - must be stopped to set coin parameters.");

    m_coinParams = coinParams;    
}

void NetworkSync::loadHeaders(const std::string& blockTreeFile, bool bCheckProofOfWork, std::function<void(const CoinQBlockTreeMem&)> callback)
{
    m_blockTreeFile = blockTreeFile;

    try
    {
        m_blockTree.loadFromFile(blockTreeFile, bCheckProofOfWork,
            [&](const CoinQBlockTreeMem& blockTree) { if (callback) callback(blockTree); });
        m_bHeadersSynched = true;
        std::stringstream status;
        status << "Best Height: " << m_blockTree.getBestHeight() << " / " << "Total Work: " << m_blockTree.getTotalWork().getDec();
        notifyStatus(status.str());
        notifyHeadersSynched();
        return;
    }
    catch (const std::exception& e)
    {
        LOGGER(error) << "NetworkSync::loadHeaders() - " << e.what() << std::endl;
        notifyBlockTreeError(e.what());
    }

    m_blockTree.clear();
    m_blockTree.setGenesisBlock(m_coinParams.genesis_block());
    m_bHeadersSynched = true;
    notifyStatus("Block tree file not found. A new one will be created.");
    notifyHeadersSynched();
}

/*
void NetworkSync::initBlockFilter()
{
    blockTree.unsubscribeAll();

    blockTree.subscribeRemoveBestChain([&](const ChainHeader& header) {
        notifyBlockTreeChanged();
        notifyRemoveBestChain(header);
    });

    blockTree.subscribeAddBestChain([&](const ChainHeader& header) {
        notifyBlockTreeChanged();
        notifyAddBestChain(header);
        uchar_vector hash = header.getHashLittleEndian();
        std::stringstream status;
        status << "Added to best chain: " << hash.getHex() << " Height: " << header.height << " ChainWork: " << header.chainWork.getDec();
        notifyStatus(status.str());
    });

    blockFilter.clear();
    blockFilter.connect([&](const ChainBlock& block) {
        uchar_vector blockHash = block.blockHeader.getHashLittleEndian();
        std::stringstream status;
        status << "Got block: " << blockHash.getHex() << " Height: " << block.height << " ChainWork: " << block.chainWork.getDec() << " # txs: " << block.txs.size();
        notifyStatus(status.str());
        for (auto& tx: block.txs) {
            notifyTx(tx);
        }

        notifyBlock(block);
        if (
            //resynching &&
             blockTree.getBestHeight() > block.height) {
            const ChainHeader& nextHeader = blockTree.getHeader(block.height + 1);
            uchar_vector blockHash = nextHeader.getHashLittleEndian();
            std::stringstream status;
            status << "Asking for block " << blockHash.getHex() << " Height: " << nextHeader.height;
            notifyStatus(status.str());
            peer.getBlock(blockHash);
        }
        else {
            resynching = false;
            notifyDoneBlockSync();
            //peer.getMempool();
        }
    });
}
*/

int NetworkSync::getBestHeight()
{
    return m_blockTree.getBestHeight();
}

void NetworkSync::syncBlocks(const std::vector<bytes_t>& locatorHashes, uint32_t startTime)
{
    if (!m_bConnected) throw std::runtime_error("NetworkSync::syncBlocks() - must connect before synching.");

/*
    boost::lock_guard<boost::mutex> lock(m_connectionMmutex);
    if (!m_bConnected) throw std::runtime_error("NetworkSync::syncBlocks() - must connect before synching.");
*/

    m_bFetchingBlocks = false;
    m_bBlocksFetched = false;

    ChainHeader header;
    bool foundHeader = false;
    for (auto& hash: locatorHashes)
    {
        try
        {
            header = m_blockTree.getHeader(hash);
            if (header.inBestChain)
            {
                foundHeader = true;
                break;
            }
            else
            {
                LOGGER(debug) << "reorg detected at height " << header.height << std::endl;
            }
        }
        catch (const std::exception& e) {
            notifyStatus(e.what());
        }
    }

    const ChainHeader& bestHeader = m_blockTree.getHeader(-1);

    int nextBlockRequestHeight;
    if (foundHeader)
    {
        nextBlockRequestHeight = header.height + 1;
    }
    else
    {
        ChainHeader header = m_blockTree.getHeaderBefore(startTime);
        nextBlockRequestHeight = header.height;
    }

    if (bestHeader.height >= nextBlockRequestHeight)
    {
        m_bFetchingBlocks = true;
        std::stringstream status;
        status << "Resynching blocks " << nextBlockRequestHeight << " - " << bestHeader.height;
        LOGGER(debug) << status.str() << std::endl;
        notifyStatus(status.str());
        notifyFetchingBlocks();
        const ChainHeader& nextBlockRequestHeader = m_blockTree.getHeader(nextBlockRequestHeight);
        uchar_vector hash = nextBlockRequestHeader.getHashLittleEndian();
        status.clear();
        status << "Asking for block " << hash.getHex();
        LOGGER(debug) << status.str() << std::endl;
        notifyStatus(status.str());
        m_lastRequestedBlockHeight = (uint32_t)nextBlockRequestHeight;
        m_peer.getFilteredBlock(hash);
    }
    else
    {
        m_bBlocksSynched = true;
        notifyBlocksSynched();
    }
}
/*
void NetworkSync::resync(int resyncHeight)
{
    if (!isConnected_) throw std::runtime_error("Must connect before resynching."); 

    stopResync();

    boost::lock_guard<boost::mutex> lock(mutex);

    resynching = true;

    initBlockFilter();

    const ChainHeader& bestHeader = blockTree.getHeader(-1); // gets the best chain's head.
    if (resyncHeight < 0) resyncHeight += bestHeader.height;

    if (bestHeader.height >= resyncHeight) {
        std::stringstream status;
        status << "Resynching blocks " << resyncHeight << " - " << bestHeader.height;
        notifyStatus(status.str());
        const ChainHeader& resyncHeader = blockTree.getHeader(resyncHeight);
        uchar_vector blockHash = resyncHeader.getHashLittleEndian();
        status.str("");
        status << "Asking for block " << blockHash.getHex();
        notifyStatus(status.str());
        lastResyncHeight = (uint32_t)resyncHeight;
        peer.getFilteredBlock(blockHash);
    }

    //peer.getMempool();
}
*/

void NetworkSync::stopSyncBlocks()
{
    if (!m_bFetchingBlocks) return;
    boost::lock_guard<boost::mutex> lock(m_syncMutex);
    m_bFetchingBlocks = false;

    // TODO: signal to indicate sync has stopped
}

void NetworkSync::start(const std::string& host, const std::string& port)
{
    LOGGER(trace) << "NetworkSync::start(" << host << ", " << port << ")" << std::endl;
    {
        if (m_bStarted) throw std::runtime_error("NetworkSync::start() - already started.");
        boost::lock_guard<boost::mutex> lock(m_startMutex);
        if (m_bStarted) throw std::runtime_error("NetworkSync::start() - already started.");

        std::string port_ = port.empty() ? m_coinParams.default_port() : port;
        m_bStarted = true;
        m_bFetchingHeaders = false;
        m_bFetchingBlocks = false;
        m_peer.set(host, port_, m_coinParams.magic_bytes(), m_coinParams.protocol_version(), "Wallet v0.1", 0, false);
        m_peer.start();
    }

    notifyStarted();
}

void NetworkSync::start(const std::string& host, int port)
{
    std::stringstream ssport;
    ssport << port;
    start(host, ssport.str());
}

void NetworkSync::stop()
{
    {
        if (!m_bStarted) return;
        boost::lock_guard<boost::mutex> lock(m_startMutex);
        if (!m_bStarted) return;

        m_bConnected = false;
        m_bStarted = false;
        m_bFetchingHeaders = false;
        m_bFetchingBlocks = false;
        m_peer.stop();
    }

    notifyStopped();
}

void NetworkSync::sendTx(Coin::Transaction& tx)
{
    m_peer.send(tx); 
}

void NetworkSync::getTx(const bytes_t& hash)
{
    m_peer.getTx(hash);
}

void NetworkSync::getTxs(const hashvector_t& hashes)
{
    m_peer.getTxs(hashes);
}

void NetworkSync::getMempool()
{
    m_peer.getMempool();
}

void NetworkSync::getFilteredBlock(const bytes_t& hash)
{
    m_peer.getFilteredBlock(hash);
}

void NetworkSync::setBloomFilter(const Coin::BloomFilter& bloomFilter)
{
    m_bloomFilter = bloomFilter;
    if (!m_bloomFilter.isSet()) return;

    Coin::FilterLoadMessage filterLoad(m_bloomFilter.getNHashFuncs(), m_bloomFilter.getNTweak(), m_bloomFilter.getNFlags(), m_bloomFilter.getFilter());
    m_peer.send(filterLoad);
    LOGGER(trace) << "Sent filter to peer." << std::endl;
}
