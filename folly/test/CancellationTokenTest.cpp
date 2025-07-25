/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <folly/CancellationToken.h>

#include <chrono>
#include <thread>

#include <folly/Optional.h>
#include <folly/portability/GTest.h>
#include <folly/synchronization/Baton.h>

using namespace folly;
using namespace std::literals::chrono_literals;

TEST(CancellationTokenTest, DefaultCancellationTokenIsNotCancellable) {
  CancellationToken t;
  EXPECT_FALSE(t.isCancellationRequested());
  EXPECT_FALSE(t.canBeCancelled());

  CancellationToken tCopy = t;
  EXPECT_FALSE(tCopy.isCancellationRequested());
  EXPECT_FALSE(tCopy.canBeCancelled());

  CancellationToken tMoved = std::move(t);
  EXPECT_FALSE(tMoved.isCancellationRequested());
  EXPECT_FALSE(tMoved.canBeCancelled());
}

TEST(CancellationTokenTest, Polling) {
  CancellationSource src;
  EXPECT_FALSE(src.isCancellationRequested());
  EXPECT_TRUE(src.canBeCancelled());

  CancellationToken token = src.getToken();
  EXPECT_FALSE(token.isCancellationRequested());
  EXPECT_TRUE(token.canBeCancelled());

  CancellationToken tokenCopy = token;
  EXPECT_FALSE(tokenCopy.isCancellationRequested());
  EXPECT_TRUE(tokenCopy.canBeCancelled());

  src.requestCancellation();
  EXPECT_TRUE(token.isCancellationRequested());
  EXPECT_TRUE(tokenCopy.isCancellationRequested());
}

TEST(CancellationTokenTest, MultiThreadedPolling) {
  CancellationSource src;

  std::thread t1{[t = src.getToken()] {
    while (!t.isCancellationRequested()) {
      std::this_thread::yield();
    }
  }};

  src.requestCancellation();

  t1.join();
}

TEST(CancellationTokenTest, TokenIsNotCancellableOnceLastSourceIsDestroyed) {
  CancellationToken token;
  {
    CancellationSource src;
    token = src.getToken();
    {
      CancellationSource srcCopy1;
      CancellationSource srcCopy2;
      EXPECT_TRUE(token.canBeCancelled());
    }
    EXPECT_TRUE(token.canBeCancelled());
  }
  EXPECT_FALSE(token.canBeCancelled());
}

TEST(
    CancellationTokenTest,
    TokenRemainsCancellableEvenOnceLastSourceIsDestroyed) {
  CancellationToken token;
  {
    CancellationSource src;
    token = src.getToken();
    {
      CancellationSource srcCopy1;
      CancellationSource srcCopy2;
      EXPECT_TRUE(token.canBeCancelled());
    }
    EXPECT_TRUE(token.canBeCancelled());
    src.requestCancellation();
  }
  EXPECT_TRUE(token.canBeCancelled());
  EXPECT_TRUE(token.isCancellationRequested());
}

TEST(CancellationTokenTest, CallbackRegistration) {
  CancellationSource src;

  bool callbackExecuted = false;
  CancellationCallback cb{src.getToken(), [&] { callbackExecuted = true; }};

  EXPECT_FALSE(callbackExecuted);

  src.requestCancellation();

  EXPECT_TRUE(callbackExecuted);
}

TEST(CancellationTokenTest, CallbackExecutesImmediatelyIfAlreadyCancelled) {
  CancellationSource src;
  src.requestCancellation();

  bool callbackExecuted = false;
  CancellationCallback cb{src.getToken(), [&] { callbackExecuted = true; }};

  EXPECT_TRUE(callbackExecuted);
}

TEST(CancellationTokenTest, CallbackShouldNotBeExecutedMultipleTimes) {
  CancellationSource src;

  int callbackExecutionCount = 0;
  CancellationCallback cb{src.getToken(), [&] { ++callbackExecutionCount; }};

  src.requestCancellation();

  EXPECT_EQ(1, callbackExecutionCount);

  src.requestCancellation();

  EXPECT_EQ(1, callbackExecutionCount);
}

TEST(CancellationTokenTest, RegisterMultipleCallbacks) {
  CancellationSource src;

  bool executed1 = false;
  CancellationCallback cb1{src.getToken(), [&] { executed1 = true; }};

  bool executed2 = false;
  CancellationCallback cb2{src.getToken(), [&] { executed2 = true; }};

  EXPECT_FALSE(executed1);
  EXPECT_FALSE(executed2);

  src.requestCancellation();

  EXPECT_TRUE(executed1);
  EXPECT_TRUE(executed2);
}

TEST(CancellationTokenTest, DeregisteredCallbacksDontExecute) {
  CancellationSource src;

  bool executed1 = false;
  bool executed2 = false;

  CancellationCallback cb1{src.getToken(), [&] { executed1 = true; }};
  {
    CancellationCallback cb2{src.getToken(), [&] { executed2 = true; }};
  }

  src.requestCancellation();

  EXPECT_TRUE(executed1);
  EXPECT_FALSE(executed2);
}

TEST(CancellationTokenTest, CallbackThatDeregistersItself) {
  CancellationSource src;
  // Check that this doesn't deadlock when a callback tries to deregister
  // itself from within the callback.
  folly::Optional<CancellationCallback> callback;
  callback.emplace(src.getToken(), [&] { callback.reset(); });
  src.requestCancellation();
}
TEST(CancellationTokenTest, ManyCallbacks) {
  // This test checks that the CancellationSource internal state is able to
  // grow to accommodate a large number of callbacks and that there are no
  // memory leaks when it's all eventually destroyed.
  CancellationSource src;
  auto addLotsOfCallbacksAndWait = [t = src.getToken()] {
    int counter = 0;
    std::vector<std::unique_ptr<CancellationCallback>> callbacks;
    for (int i = 0; i < 100; ++i) {
      callbacks.push_back(std::make_unique<CancellationCallback>(t, [&] {
        ++counter;
      }));
    }

    Baton<> baton;
    CancellationCallback cb{t, [&] { baton.post(); }};
    baton.wait();
  };

  std::thread t1{addLotsOfCallbacksAndWait};
  std::thread t2{addLotsOfCallbacksAndWait};
  std::thread t3{addLotsOfCallbacksAndWait};
  std::thread t4{addLotsOfCallbacksAndWait};

  src.requestCancellation();

  t1.join();
  t2.join();
  t3.join();
  t4.join();
}

TEST(CancellationTokenTest, ManyConcurrentCallbackAddRemove) {
  auto runTest = [](CancellationToken ct) {
    auto cb = [] { std::this_thread::sleep_for(1ms); };
    while (!ct.isCancellationRequested()) {
      CancellationCallback cb1{ct, cb};
      CancellationCallback cb2{ct, cb};
      CancellationCallback cb3{ct, cb};
      CancellationCallback cb5{ct, cb};
      CancellationCallback cb6{ct, cb};
      CancellationCallback cb7{ct, cb};
      CancellationCallback cb8{ct, cb};
    }
  };

  CancellationSource src;

  std::vector<std::thread> threads;
  for (int i = 0; i < 10; ++i) {
    threads.emplace_back([&, t = src.getToken()] { runTest(t); });
  }

  std::this_thread::sleep_for(1s);

  src.requestCancellation();

  for (auto& t : threads) {
    t.join();
  }
}

TEST(CancellationTokenTest, NonCancellableSource) {
  CancellationSource src = CancellationSource::invalid();
  CHECK(!src.canBeCancelled());
  CHECK(!src.isCancellationRequested());
  CHECK(!src.requestCancellation());
  CHECK(!src.isCancellationRequested());
  CHECK(!src.canBeCancelled());

  auto token = src.getToken();
  CHECK(!src.canBeCancelled());
  CHECK(!src.isCancellationRequested());
  CHECK(token == CancellationToken{});
}

TEST(CancellationTokenTest, MergedToken) {
  CancellationSource src1, src2;

  auto token = cancellation_token_merge(src1.getToken(), src2.getToken());

  EXPECT_TRUE(token.canBeCancelled());
  EXPECT_FALSE(token.isCancellationRequested());

  bool callbackExecuted = false;
  CancellationCallback cb{token, [&] { callbackExecuted = true; }};

  EXPECT_FALSE(callbackExecuted);
  EXPECT_FALSE(token.isCancellationRequested());

  src1.requestCancellation();

  EXPECT_TRUE(callbackExecuted);
  EXPECT_TRUE(token.isCancellationRequested());

  src2.requestCancellation();

  EXPECT_TRUE(callbackExecuted);
  EXPECT_TRUE(token.isCancellationRequested());

  token = cancellation_token_merge();
  EXPECT_FALSE(token.canBeCancelled());

  token = cancellation_token_merge(CancellationToken());
  EXPECT_FALSE(token.canBeCancelled());
}

TEST(CancellationTokenTest, Merging1TokenIsEfficient) {
  CancellationSource src;
  CancellationToken tok = src.getToken();
  EXPECT_TRUE(tok == cancellation_token_merge(tok));
}

// The next bunch of `MergedToken_` tests aims to exercise the distinct
// createMove / createCopy / createCopyMove construction paths.  They're all
// supposed to behave the same.  The coverage isn't combinatorially
// exhaustive to avoid exhausting the reader :)

struct TestCallbackExecutedOnExit {
  bool executed_{false};
  CancellationCallback cb_;
  explicit TestCallbackExecutedOnExit(const CancellationToken& token)
      : cb_{token, [this] { executed_ = true; }} {}
  ~TestCallbackExecutedOnExit() { CHECK(executed_); }
};

TEST(CancellationTokenTest, MergedToken_LValA_LValB_cancelA) {
  CancellationSource srcA, srcB;
  auto tokA = srcA.getToken(), tokB = srcB.getToken();
  auto merged = cancellation_token_merge(tokA, tokB);
  TestCallbackExecutedOnExit testCb{merged};
  EXPECT_TRUE(tokA.canBeCancelled());
  EXPECT_TRUE(tokB.canBeCancelled());
  srcA.requestCancellation();
  EXPECT_TRUE(merged.isCancellationRequested());
}

TEST(CancellationTokenTest, MergedToken_LValA_LValB_cancelB) {
  CancellationSource srcA, srcB;
  auto tokA = srcA.getToken(), tokB = srcB.getToken();
  auto merged = cancellation_token_merge(tokA, tokB);
  TestCallbackExecutedOnExit testCb{merged};
  srcB.requestCancellation();
  EXPECT_TRUE(merged.isCancellationRequested());
}

TEST(CancellationTokenTest, MergedToken_LValA_RValB_cancelA) {
  CancellationSource srcA, srcB;
  auto tokA = srcA.getToken(), tokB = srcB.getToken();
  auto merged = cancellation_token_merge(tokA, std::move(tokB));
  EXPECT_TRUE(tokA.canBeCancelled());
  EXPECT_FALSE(tokB.canBeCancelled());
  TestCallbackExecutedOnExit testCb{merged};
  srcA.requestCancellation();
  EXPECT_TRUE(merged.isCancellationRequested());
}

TEST(CancellationTokenTest, MergedToken_LValA_RValB_cancelB) {
  CancellationSource srcA, srcB;
  auto tokA = srcA.getToken(), tokB = srcB.getToken();
  auto merged = cancellation_token_merge(tokA, std::move(tokB));
  TestCallbackExecutedOnExit testCb{merged};
  srcB.requestCancellation();
  EXPECT_TRUE(merged.isCancellationRequested());
}

TEST(CancellationTokenTest, MergedToken_RValA_RValB_cancelA) {
  CancellationSource srcA, srcB;
  auto tokA = srcA.getToken(), tokB = srcB.getToken();
  auto merged = cancellation_token_merge(std::move(tokA), std::move(tokB));
  EXPECT_FALSE(tokA.canBeCancelled());
  EXPECT_FALSE(tokB.canBeCancelled());
  TestCallbackExecutedOnExit testCb{merged};
  srcA.requestCancellation();
  EXPECT_TRUE(merged.isCancellationRequested());
}

TEST(CancellationTokenTest, MergedTokenDeduplication) {
  CancellationSource src1, src2;
  auto tok1 = src1.getToken();

  // None of these allocate a "merging state" because there's only one
  // nonempty token.
  EXPECT_TRUE(cancellation_token_merge(tok1) == tok1);
  EXPECT_TRUE(cancellation_token_merge(CancellationToken(), tok1) == tok1);
  EXPECT_TRUE(
      cancellation_token_merge(
          CancellationToken(), tok1, CancellationToken()) == tok1);
  EXPECT_TRUE(
      cancellation_token_merge(
          CancellationToken(), tok1, CancellationToken(), tok1) == tok1);

  // A new token with the same state is still deduplicated.
  EXPECT_TRUE(src1.getToken() == tok1);
  EXPECT_TRUE(
      cancellation_token_merge(
          src1.getToken(), tok1, CancellationToken(), tok1) == tok1);

  // Combining multiple distinct tokens allocates a new state
  auto tok2 = src2.getToken();
  auto tok12 = cancellation_token_merge(tok2, tok1, CancellationToken(), tok1);
  EXPECT_FALSE(tok12 == tok1);
  EXPECT_FALSE(tok12 == tok2);
}

TEST(CancellationTokenTest, MergedTokenDeduplicationNotObservable_RValA_RValA) {
  CancellationSource src;
  auto tok1 = src.getToken(), tok2 = src.getToken();
  EXPECT_TRUE(tok1.canBeCancelled());
  EXPECT_TRUE(tok2.canBeCancelled());
  auto merged = cancellation_token_merge(std::move(tok1), std::move(tok2));
  EXPECT_TRUE(src.getToken() == merged);
  // Both were moved out, even though only 1 was used.
  EXPECT_FALSE(tok1.canBeCancelled());
  EXPECT_FALSE(tok2.canBeCancelled());
}

TEST(
    CancellationTokenTest,
    MergedTokenDeduplicationNotObservable_RValA_LValA_RValA) {
  CancellationSource src;
  auto tok1 = src.getToken(), tok2 = src.getToken(), tok3 = src.getToken();
  auto merged =
      cancellation_token_merge(std::move(tok1), tok2, std::move(tok3));
  EXPECT_TRUE(src.getToken() == merged);
  // Both r-values were moved out, even though only 1 was used.
  EXPECT_FALSE(tok1.canBeCancelled());
  EXPECT_FALSE(tok3.canBeCancelled());
}

// Covers the "destroy merging state" branch in `unlockAndDecrementTokenCount`
TEST(CancellationTokenTest, MergedTokenDestroyedViaCallback) {
  CancellationToken mergedTwice;
  CancellationSource srcOut;
  {
    CancellationSource srcIn;
    auto merged = cancellation_token_merge(srcOut.getToken(), srcIn.getToken());
    // This second merging state has a `CancellationCallback` that keeps the
    // first merging state alive even after its own callbacks are dead.
    // That causes the first state to be destroyed on an atypical branch.
    mergedTwice = cancellation_token_merge(merged, srcIn.getToken());
  }
  EXPECT_TRUE(mergedTwice.canBeCancelled());
}

TEST(CancellationTokenTest, TokenWithData) {
  struct Guard {
    int& counter;
    explicit Guard(int& c) : counter(c) {}
    ~Guard() { ++counter; }
  };
  int counter = 0;

  {
    CancellationToken token;
    {
      auto [source, data] =
          CancellationSource::create(detail::WithDataTag<Guard>{}, counter);
      EXPECT_EQ(counter, 0);
      token = source.getToken();
      EXPECT_EQ(counter, 0);
    }
    EXPECT_EQ(counter, 0);
  }
  EXPECT_EQ(counter, 1);
}
