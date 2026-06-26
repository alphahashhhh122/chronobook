// ChronoBook test suite - assert-based and dependency-free.
// Covers matching, replay, persistence, queues, and benchmark helpers.
#include "core/Order.h"
#include "core/OrderPool.h"
#include "core/PriceLevel.h"
#include "core/OrderBook.h"
#include "matching/MatchingEngine.h"
#include "feed/BinaryProtocol.h"
#include "feed/FeedParser.h"
#include "feed/FeedGenerator.h"
#include "infra/ThreadSafeQueue.h"
#include "infra/SPSCRingBuffer.h"
#include "infra/FutexSemaphore.h"
#include "journal/FillJournal.h"
#include "journal/DurabilityPipeline.h"
#include "matching/ReferenceMatcher.h"
#include "replay/LatencyHistogram.h"
#include "replay/ReplayStats.h"
#include "replay/ReplayEngine.h"
#include "replay/RecoveryReconciler.h"
#include "store/TradeStore.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>
#include <cmath>

using namespace chronobook;

static int g_tests = 0;
#define CHECK(cond) do { ++g_tests; if(!(cond)){ printf("FAIL line %d: %s\n", __LINE__, #cond); std::abort(); } } while(0)

static Order* mk(OrderPool& p, uint64_t id, Side s, OrderType t, uint32_t px, uint32_t q){
    Order* o=p.allocate(); o->orderId=id; o->side=s; o->type=t; o->price=px; o->qty=q; o->filledQty=0; return o;
}

static void test_order() {
    CHECK(sizeof(Order)==48);
    Order o; o.qty=100; o.filledQty=40; CHECK(o.remainingQty()==60); CHECK(!o.isFilled());
    o.filledQty=100; CHECK(o.isFilled()); CHECK(o.remainingQty()==0);
    o.filledQty=200; CHECK(o.remainingQty()==0);                 // clamp, no underflow
    o.setSymbol("AAPL"); CHECK(o.getSymbol()=="AAPL");
}
static void test_pool() {
    OrderPool p(4);
    CHECK(p.capacity()==4 && p.freeCount()==4 && p.allocatedCount()==0);
    Order* a=p.allocate(); Order* b=p.allocate();
    CHECK(p.allocatedCount()==2 && p.freeCount()==2);
    p.deallocate(a); CHECK(p.allocatedCount()==1);
    Order* c=p.allocate(); CHECK(c==a);                          // LIFO reuse
    Order* d=p.allocate(); Order* e=p.allocate();
    CHECK(d && e); CHECK(p.allocate()==nullptr);                 // exhausted
    (void)b;(void)c;
}

static void test_pricelevel() {
    OrderPool p(8); PriceLevel lvl;
    Order* o1=mk(p,1,Side::BUY,OrderType::LIMIT,100,10);
    Order* o2=mk(p,2,Side::BUY,OrderType::LIMIT,100,20);
    Order* o3=mk(p,3,Side::BUY,OrderType::LIMIT,100,30);
    lvl.addOrder(o1); lvl.addOrder(o2); lvl.addOrder(o3);
    CHECK(lvl.getOrderCount()==3 && lvl.getTotalQty()==60);
    CHECK(lvl.front()==o1);                                      // FIFO head
    lvl.removeOrder(o2);                                         // middle removal
    CHECK(lvl.getOrderCount()==2 && lvl.getTotalQty()==40);
    CHECK(lvl.front()==o1 && o1->next==o3 && o3->prev==o1);
    lvl.removeOrder(o1);                                         // head removal
    CHECK(lvl.front()==o3);
    lvl.removeOrder(o3);                                         // last removal
    CHECK(lvl.isEmpty());
}
static void test_orderbook() {
    OrderPool p(16); OrderBook b;
    b.addOrder(mk(p,1,Side::BUY,OrderType::LIMIT,100,10));
    b.addOrder(mk(p,2,Side::BUY,OrderType::LIMIT,101,20));
    b.addOrder(mk(p,3,Side::SELL,OrderType::LIMIT,105,30));
    CHECK(b.getBestBid()==101 && b.getBestAsk()==105);
    CHECK(b.getBestBidQty()==20 && b.getBestAskQty()==30);
    CHECK(b.getSpread()==4);
    Order* c=b.cancelOrder(2); CHECK(c && c->orderId==2);
    CHECK(b.getBestBid()==100);                                 // 101 level erased
    p.deallocate(c);
}
static void test_duplicate_order_id_rejected() {
    OrderPool p(16); MatchingEngine e(p);
    e.processOrder(mk(p,1,Side::BUY,OrderType::LIMIT,100,10),1);
    e.processOrder(mk(p,1,Side::BUY,OrderType::LIMIT,101,20),2);
    CHECK(e.getBook().getBestBid()==100);
    CHECK(e.getBook().getBestBidQty()==10);
    CHECK(p.allocatedCount()==1);
}

static void test_match_partial_and_fifo() {
    OrderPool p(64); MatchingEngine e(p);
    e.processOrder(mk(p,1,Side::SELL,OrderType::LIMIT,100,40),1);   // ask 100x40 (older)
    e.processOrder(mk(p,2,Side::SELL,OrderType::LIMIT,100,40),2);   // ask 100x40 (newer)
    e.processOrder(mk(p,3,Side::BUY ,OrderType::LIMIT,100,50),3);   // buy 50 crosses
    auto f=e.drainFills();
    CHECK(f.size()==2);
    CHECK(f[0].sellOrderId==1 && f[0].qty==40);                 // FIFO: older first
    CHECK(f[1].sellOrderId==2 && f[1].qty==10);                 // then 10 from newer
    CHECK(e.getBook().getBestAsk()==100 && e.getBook().getBestAskQty()==30);
    CHECK(e.getBook().getBestBid()==0);                         // buy fully filled, no rest
}
static void test_match_rest_and_nomatch() {
    OrderPool p(64); MatchingEngine e(p);
    e.processOrder(mk(p,1,Side::SELL,OrderType::LIMIT,105,10),1);
    e.processOrder(mk(p,2,Side::BUY ,OrderType::LIMIT,100,10),2);   // 100<105 no cross
    CHECK(e.drainFills().empty());
    CHECK(e.getBook().getBestBid()==100 && e.getBook().getBestAsk()==105);
}
static void test_market_and_ioc() {
    OrderPool p(64); MatchingEngine e(p);
    e.processOrder(mk(p,1,Side::SELL,OrderType::LIMIT,100,10),1);
    e.processOrder(mk(p,2,Side::SELL,OrderType::LIMIT,101,10),2);
    e.processOrder(mk(p,3,Side::BUY ,OrderType::MARKET,0,15),3);    // sweep both levels
    auto f=e.drainFills(); CHECK(f.size()==2 && f[0].price==100 && f[1].price==101);
    CHECK(e.getBook().getBestAsk()==101 && e.getBook().getBestAskQty()==5);
    CHECK(e.getBook().getBestBid()==0);                            // market never rests
    // IOC: partial fill, residual cancelled (does not rest)
    e.processOrder(mk(p,4,Side::BUY,OrderType::IOC,101,100),4);
    auto f2=e.drainFills(); CHECK(f2.size()==1 && f2[0].qty==5);
    CHECK(e.getBook().getBestBid()==0 && e.getBook().getBestAsk()==0);
}
static void test_cancel_after_partial() {
    OrderPool p(64); MatchingEngine e(p);
    e.processOrder(mk(p,1,Side::SELL,OrderType::LIMIT,100,50),1);
    e.processOrder(mk(p,2,Side::BUY ,OrderType::LIMIT,100,20),2);   // partial: ask now 30
    CHECK(e.getBook().getBestAskQty()==30);
    CHECK(e.cancelOrder(1));                                        // cancel partially-filled resting
    CHECK(e.getBook().getBestAsk()==0);
    CHECK(!e.cancelOrder(999));                                     // unknown id
}

static void test_protocol_roundtrip() {
    CHECK(sizeof(FeedMessage)==40);
    FeedMessage a=makeAdd(7,42,Side::SELL,OrderType::IOC,10050,99,0xABCD);
    std::byte buf[kFeedMessageSize]; encodeMessage(a,buf);
    FeedMessage b=decodeMessage(buf);
    CHECK(b.sequence==7 && b.orderId==42 && b.price==10050 && b.qty==99);
    CHECK(b.msgType==MsgType::ADD && b.side==(uint8_t)Side::SELL && b.orderType==(uint8_t)OrderType::IOC);
    CHECK(b.symbolPacked==0xABCD);
    FeedMessage c=makeCancel(8,42); FeedMessage d=makeModify(9,42,10060,55);
    CHECK(c.msgType==MsgType::CANCEL && d.msgType==MsgType::MODIFY && d.price==10060 && d.qty==55);
}
static void test_parser_and_partial_frame() {
    std::vector<FeedMessage> msgs{ makeAdd(1,1,Side::BUY,OrderType::LIMIT,100,10,0),
                                   makeCancel(2,1), makeModify(3,1,101,5) };
    auto bytes=FeedParser::encodeAll(msgs);
    size_t consumed=0; auto parsed=FeedParser::parseBuffer(bytes.data(),bytes.size(),&consumed);
    CHECK(parsed.size()==3 && consumed==bytes.size());
    // append a 5-byte partial frame: parser must ignore it, report consumed==full frames
    bytes.resize(bytes.size()+5);
    parsed=FeedParser::parseBuffer(bytes.data(),bytes.size(),&consumed);
    CHECK(parsed.size()==3 && consumed==3*kFeedMessageSize);
}
static void test_queue_basic() {
    ThreadSafeQueue<int> q;
    q.push(1); q.push(2); q.push(3);
    CHECK(q.size()==3);
    CHECK(*q.pop()==1 && *q.pop()==2);                             // FIFO
    CHECK(*q.tryPop()==3 && !q.tryPop());
    q.push(9); q.close();
    CHECK(*q.pop()==9);                                            // drains before sentinel
    CHECK(!q.pop());                                               // closed + empty -> nullopt
}
static void test_queue_concurrent() {
    ThreadSafeQueue<int> q;
    const int N=10000; long long sum=0;
    std::thread prod([&]{ for(int i=1;i<=N;++i) q.push(i); q.close(); });
    std::thread cons([&]{ while(auto v=q.pop()) sum+=*v; });
    prod.join(); cons.join();
    CHECK(sum==(long long)N*(N+1)/2);                              // every item exactly once
}
static void test_generator_determinism() {
    FeedConfig cfg; cfg.numMessages=5000; cfg.seed=123;
    auto a=FeedGenerator(cfg).generate();
    auto b=FeedGenerator(cfg).generate();
    CHECK(a.size()==b.size());
    bool same=true; for(size_t i=0;i<a.size();++i){
        if(a[i].sequence!=b[i].sequence||a[i].orderId!=b[i].orderId||a[i].price!=b[i].price||
           a[i].qty!=b[i].qty||a[i].msgType!=b[i].msgType||a[i].side!=b[i].side){ same=false; break; }
    }
    CHECK(same);                                                  // same seed -> identical
    FeedConfig cfg2=cfg; cfg2.seed=124; auto c=FeedGenerator(cfg2).generate();
    bool diff=false; for(size_t i=0;i<a.size();++i) if(a[i].orderId!=c[i].orderId){diff=true;break;}
    CHECK(diff);                                                  // different seed -> different
}

static void test_running_stats() {
    RunningStats s; for(double x:{2.0,4.0,4.0,4.0,5.0,5.0,7.0,9.0}) s.add(x);
    CHECK(std::fabs(s.mean-5.0)<1e-9);
    CHECK(std::fabs(s.variance()-32.0/7.0)<1e-9);                  // sample variance
    CHECK(s.min()==2.0 && s.max()==9.0 && s.n==8);
}
static void test_analytics_imbalance() {
    OrderPool p(64); MatchingEngine e(p);
    e.processOrder(mk(p,1,Side::BUY ,OrderType::LIMIT,100,80),1);
    e.processOrder(mk(p,2,Side::SELL,OrderType::LIMIT,101,20),2);
    ImbalanceTracker imb; imb.update(e.getBook());
    CHECK(std::fabs(imb.current()-0.6)<1e-9);                      // (80-20)/(80+20)
    QueueDepthTracker qd; qd.update(e.getBook());
    CHECK(qd.bidQty()==80 && qd.askQty()==20);
    SpreadTracker sp; sp.update(e.getBook()); CHECK(sp.current()==1);
}
static ReplayStats runFeed(const std::vector<FeedMessage>& feed, uint64_t& bb, uint64_t& ba){
    OrderPool p(200000); MatchingEngine e(p); ReplayEngine r(e,p);
    auto st=r.run(feed, ReplayMode::NORMAL);
    bb=e.getBook().getBestBid(); ba=e.getBook().getBestAsk();
    return st;
}
static void test_replay_determinism() {
    FeedConfig cfg; cfg.numMessages=20000; cfg.seed=777;
    auto feed=FeedGenerator(cfg).generate();
    uint64_t bb1,ba1,bb2,ba2;
    auto s1=runFeed(feed,bb1,ba1);
    auto s2=runFeed(feed,bb2,ba2);
    CHECK(s1.fills==s2.fills && s1.matchedVolume==s2.matchedVolume);
    CHECK(bb1==bb2 && ba1==ba2);                                  // identical end state
    CHECK(s1.messages==20000);
    CHECK(s1.adds+s1.cancels+s1.modifies==s1.messages);
}
static void test_replay_modify_preserves_side() {
    std::vector<FeedMessage> feed{
        makeAdd(1,1,Side::SELL,OrderType::LIMIT,100,10,0),
        makeModify(2,1,105,10),
        makeAdd(3,2,Side::BUY,OrderType::LIMIT,104,10,0)
    };
    OrderPool p(64); MatchingEngine e(p); ReplayEngine r(e,p);
    auto st=r.run(feed, ReplayMode::NORMAL);
    CHECK(st.fills==0);
    CHECK(e.getBook().getBestAsk()==105);
    CHECK(e.getBook().getBestBid()==104);

    auto cross=r.run(std::vector<FeedMessage>{
        makeAdd(4,3,Side::BUY,OrderType::LIMIT,105,10,0)
    }, ReplayMode::NORMAL);
    CHECK(cross.fills==1);
    CHECK(e.getBook().getBestAsk()==0);
}
static void test_replay_max_throughput() {
    FeedConfig cfg; cfg.numMessages=50000; cfg.seed=5;
    auto feed=FeedGenerator(cfg).generate();
    OrderPool p(200000); MatchingEngine e(p); ReplayEngine r(e,p);
    auto st=r.run(feed, ReplayMode::MAX, /*warmup=*/1000);
    CHECK(st.messages==50000);
    CHECK(st.throughputMsgsPerSec()>0.0);
}
// producer-consumer feeding the engine == single-threaded result
static void test_producer_consumer_equiv() {
    FeedConfig cfg; cfg.numMessages=15000; cfg.seed=2024;
    auto feed=FeedGenerator(cfg).generate();
    uint64_t bbA,baA; auto stA=runFeed(feed,bbA,baA);            // single-threaded baseline

    OrderPool p(200000); MatchingEngine e(p); ReplayEngine r(e,p);
    ThreadSafeQueue<FeedMessage> q;
    std::thread producer([&]{ for(auto& m:feed) q.push(m); q.close(); });
    ReplayStats stB;
    std::thread consumer([&]{
        std::vector<FeedMessage> one(1);
        while(auto m=q.pop()){ one[0]=*m; auto s=r.run(one, ReplayMode::NORMAL); stB.fills+=s.fills; stB.matchedVolume+=s.matchedVolume; stB.messages+=s.messages; }
    });
    producer.join(); consumer.join();
    CHECK(stB.messages==feed.size());
    CHECK(stB.fills==stA.fills && stB.matchedVolume==stA.matchedVolume);
    CHECK(e.getBook().getBestBid()==bbA && e.getBook().getBestAsk()==baA);
}

// lock-free SPSC handoff.
static void test_spsc_ring_concurrent() {
    SPSCRingBuffer<int> q(256);
    const int N = 50000;
    long long sum = 0;
    std::thread prod([&] {
        for (int i = 1; i <= N; ++i) {
            while (!q.tryPush(i)) std::this_thread::yield();
        }
    });
    std::thread cons([&] {
        for (int seen = 0; seen < N;) {
            int v = 0;
            if (q.tryPop(v)) {
                sum += v;
                ++seen;
            } else {
                std::this_thread::yield();
            }
        }
    });
    prod.join();
    cons.join();
    CHECK(sum == (long long)N * (N + 1) / 2);
    CHECK(q.empty());
}

static void test_latency_histogram() {
    LatencyHistogram h;
    for (uint64_t i = 1; i <= 1000; ++i) h.record(i);
    auto s = h.summarize();
    CHECK(h.count() == 1000);
    CHECK(s.p50 == 500 && s.p99 == 990 && s.p999 == 999 && s.max == 1000);
}

// journal, SQLite projection, transaction rollback, read pool.
static void cleanup_db(const char* path);

static void test_fill_journal_roundtrip() {
    const char* path = "chronobook_test.journal";
    std::remove(path);
    {
        FillJournal journal(path, 8);
        journal.append(Fill{1,2,100,10,7});
        journal.append(Fill{3,4,101,20,8});
        journal.flush();
    }
    {
        FillJournal journal(path, 8);
        CHECK(journal.recordsWritten() == 2);
        journal.append(Fill{5,6,102,30,9});
        journal.flush();
    }
    auto records = FillJournal::replay(path);
    CHECK(records.size() == 3);
    CHECK(records[0].buyOrderId == 1 && records[0].sellOrderId == 2);
    CHECK(records[1].price == 101 && records[1].qty == 20);
    CHECK(records[2].sellOrderId == 6 && records[2].timestamp == 9);
    std::remove(path);
}

static void test_durability_pipeline_spsc_to_journal_and_store() {
    const char* journalPath = "chronobook_pipeline.journal";
    const char* storePath = "chronobook_pipeline.db";
    std::remove(journalPath);
    cleanup_db(storePath);
    {
        DurabilityPipeline pipe(journalPath, storePath, 16, 16, 2);
        pipe.start();
        std::vector<Fill> fills{{1,2,100,10,1}, {3,4,105,20,2}};
        CHECK(pipe.publishBatch(fills)==2);
        pipe.stop();
        CHECK(pipe.error().empty());
        CHECK(pipe.persisted()==2);
    }
    auto records = FillJournal::replay(journalPath);
    CHECK(records.size()==2);
    CHECK(records[0].price==100 && records[1].qty==20);
    TradeStore store(storePath);
    CHECK(store.tradeCount()==2);
    CHECK(std::fabs(store.vwap(1,2)-103.33333333333333)<1e-9);
    std::remove(journalPath);
    cleanup_db(storePath);
}

static void test_recovery_reconciles_feed_journal_and_store() {
    const char* journalPath = "chronobook_recovery.journal";
    const char* storePath = "chronobook_recovery.db";
    std::remove(journalPath);
    cleanup_db(storePath);

    std::vector<FeedMessage> feed{
        makeAdd(1,1,Side::SELL,OrderType::LIMIT,100,40,0),
        makeAdd(2,2,Side::SELL,OrderType::LIMIT,101,30,0),
        makeAdd(3,3,Side::BUY, OrderType::LIMIT,101,50,0),
        makeAdd(4,4,Side::BUY, OrderType::LIMIT, 99,10,0),
        makeCancel(5,4),
        makeAdd(6,5,Side::BUY, OrderType::MARKET,0,20,0)
    };

    auto recovered = RecoveryReconciler::replayFills(feed, 64);
    CHECK(recovered.size() == 3);
    {
        FillJournal journal(journalPath, 16);
        journal.appendBatch(recovered);
    }
    TradeStore store(storePath);
    store.insertBatch(recovered);

    auto report = RecoveryReconciler::reconcile(feed, journalPath, storePath, 64);
    CHECK(report.ok());
    CHECK(report.replayFills == 3);
    CHECK(report.journalFills == 3);
    CHECK(report.storeTrades == 3);
    CHECK(std::fabs(report.replayVwap - report.storeVwap) < 1e-9);

    std::remove(journalPath);
    cleanup_db(storePath);
}

static void cleanup_db(const char* path) {
    std::remove(path);
    std::string wal = std::string(path) + "-wal";
    std::string shm = std::string(path) + "-shm";
    std::remove(wal.c_str());
    std::remove(shm.c_str());
}

static void test_trade_store_atomicity_and_queries() {
    const char* path = "chronobook_test.db";
    cleanup_db(path);
    TradeStore store(path);
    std::vector<Fill> fills{{1,2,100,10,1}, {3,4,200,30,2}};
    store.insertBatch(fills);
    CHECK(store.tradeCount() == 2);
    CHECK(std::fabs(store.vwap(1, 2) - 175.0) < 1e-9);
    bool threw = false;
    try {
        store.insertBatch(std::vector<Fill>{{5,6,999,99,3}}, true);
    } catch (const SQLiteError&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(store.tradeCount() == 2);
    CHECK(store.explainPlan().find("idx_trades_ts") != std::string::npos ||
          store.explainPlan().find("SEARCH") != std::string::npos);
    cleanup_db(path);
}

static void test_connection_pool_lease_move_and_timeout() {
    const char* path = "chronobook_pool.db";
    cleanup_db(path);
    TradeStore seed(path);
    seed.insertBatch(std::vector<Fill>{{1,2,100,10,1}});
    ConnectionPool pool(path, 1);
    auto lease = pool.acquire();
    CHECK((bool)lease);
    auto missing = pool.tryAcquireFor(std::chrono::milliseconds(10));
    CHECK(!missing);
    ConnectionLease moved = std::move(lease);
    CHECK((bool)moved);
    moved.reset();
    CHECK(pool.available() == 1);
    cleanup_db(path);
}

// differential oracle over a hand-computable sequence.
static void test_reference_matcher_diff() {
    std::vector<FeedMessage> feed{
        makeAdd(1,1,Side::SELL,OrderType::LIMIT,100,40,0),
        makeAdd(2,2,Side::SELL,OrderType::LIMIT,100,40,0),
        makeAdd(3,3,Side::BUY, OrderType::LIMIT,100,50,0),
        makeAdd(4,4,Side::BUY, OrderType::LIMIT, 99,10,0),
        makeCancel(5,4),
        makeAdd(6,5,Side::BUY, OrderType::MARKET,0,30,0),
        makeAdd(7,6,Side::SELL,OrderType::LIMIT,105,10,0),
        makeModify(8,6,106,10),
        makeAdd(9,7,Side::BUY, OrderType::LIMIT,106,10,0)
    };

    OrderPool pool(64);
    MatchingEngine engine(pool);
    std::vector<Fill> fast;
    for (const auto& msg : feed) {
        if (msg.msgType == MsgType::ADD) {
            Order* o = pool.allocate();
            o->orderId = msg.orderId; o->symbolPacked = msg.symbolPacked;
            o->price = msg.price; o->qty = msg.qty; o->filledQty = 0;
            o->side = static_cast<Side>(msg.side);
            o->type = static_cast<OrderType>(msg.orderType);
            engine.processOrder(o, msg.sequence);
        } else if (msg.msgType == MsgType::CANCEL) {
            engine.cancelOrder(msg.orderId);
        } else if (msg.msgType == MsgType::MODIFY) {
            engine.modifyOrder(msg.orderId, msg.price, msg.qty, msg.sequence);
        }
        auto batch = engine.drainFills();
        fast.insert(fast.end(), batch.begin(), batch.end());
    }

    ReferenceMatcher ref;
    auto slow = ref.run(feed);
    CHECK(fast.size() == slow.size());
    for (size_t i = 0; i < fast.size(); ++i) {
        CHECK(fast[i].buyOrderId == slow[i].buyOrderId);
        CHECK(fast[i].sellOrderId == slow[i].sellOrderId);
        CHECK(fast[i].price == slow[i].price);
        CHECK(fast[i].qty == slow[i].qty);
        CHECK(fast[i].timestamp == slow[i].timestamp);
    }
}

int main(){
    test_order(); test_pool();
    test_pricelevel(); test_orderbook();
    test_duplicate_order_id_rejected();
    test_match_partial_and_fifo(); test_match_rest_and_nomatch();
    test_market_and_ioc(); test_cancel_after_partial();
    test_protocol_roundtrip(); test_parser_and_partial_frame();
    test_queue_basic(); test_queue_concurrent();
    test_generator_determinism();
    test_running_stats(); test_analytics_imbalance();
    test_replay_determinism(); test_replay_modify_preserves_side();
    test_replay_max_throughput();
    test_producer_consumer_equiv();
    test_spsc_ring_concurrent(); test_latency_histogram();
    test_fill_journal_roundtrip();
    test_durability_pipeline_spsc_to_journal_and_store();
    test_recovery_reconciles_feed_journal_and_store();
    test_trade_store_atomicity_and_queries();
    test_connection_pool_lease_move_and_timeout();
    test_reference_matcher_diff();
    printf("ALL %d CHECKS PASSED\n", g_tests);
    return 0;
}
