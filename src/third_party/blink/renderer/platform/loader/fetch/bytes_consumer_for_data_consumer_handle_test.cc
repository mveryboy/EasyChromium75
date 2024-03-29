// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/platform/loader/fetch/bytes_consumer_for_data_consumer_handle.h"

#include <memory>

#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/renderer/platform/loader/fetch/bytes_consumer.h"
#include "third_party/blink/renderer/platform/loader/testing/replaying_web_data_consumer_handle.h"
#include "third_party/blink/renderer/platform/scheduler/test/fake_task_runner.h"
#include "third_party/blink/renderer/platform/testing/testing_platform_support_with_mock_scheduler.h"
#include "third_party/blink/renderer/platform/testing/unit_test_helpers.h"

namespace blink {

namespace {

using Checkpoint = testing::StrictMock<testing::MockFunction<void(int)>>;
using Result = BytesConsumer::Result;
using testing::ByMove;
using testing::InSequence;
using testing::Return;

class BytesConsumerForDataConsumerHandleTest : public testing::Test {
 protected:
  using Command = ReplayingWebDataConsumerHandle::Command;
  ~BytesConsumerForDataConsumerHandleTest() override {
    ThreadState::Current()->CollectAllGarbageForTesting();
  }

  scoped_refptr<base::SingleThreadTaskRunner> GetTaskRunner() {
    return platform_->test_task_runner();
  }

  ScopedTestingPlatformSupport<TestingPlatformSupportWithMockScheduler>
      platform_;
};

class MockBytesConsumerClient
    : public GarbageCollectedFinalized<MockBytesConsumerClient>,
      public BytesConsumer::Client {
  USING_GARBAGE_COLLECTED_MIXIN(MockBytesConsumerClient);

 public:
  static MockBytesConsumerClient* Create() {
    return MakeGarbageCollected<testing::StrictMock<MockBytesConsumerClient>>();
  }
  MOCK_METHOD0(OnStateChange, void());
  String DebugName() const override { return "MockBytesConsumerClient"; }

 protected:
  MockBytesConsumerClient() {}
};

class MockDataConsumerHandle final : public WebDataConsumerHandle {
 public:
  class MockReaderProxy : public GarbageCollectedFinalized<MockReaderProxy> {
   public:
    MOCK_METHOD3(BeginRead,
                 WebDataConsumerHandle::Result(const void**,
                                               WebDataConsumerHandle::Flags,
                                               size_t*));
    MOCK_METHOD1(EndRead, WebDataConsumerHandle::Result(size_t));

    void Trace(blink::Visitor* visitor) {}
  };

  MockDataConsumerHandle() : proxy_(MakeGarbageCollected<MockReaderProxy>()) {}
  MockReaderProxy* Proxy() { return proxy_; }
  const char* DebugName() const override { return "MockDataConsumerHandle"; }

 private:
  class Reader final : public WebDataConsumerHandle::Reader {
   public:
    explicit Reader(MockReaderProxy* proxy) : proxy_(proxy) {}
    Result BeginRead(const void** buffer,
                     Flags flags,
                     size_t* available) override {
      return proxy_->BeginRead(buffer, flags, available);
    }
    Result EndRead(size_t read_size) override {
      return proxy_->EndRead(read_size);
    }

   private:
    Persistent<MockReaderProxy> proxy_;
  };

  std::unique_ptr<WebDataConsumerHandle::Reader> ObtainReader(
      Client*,
      scoped_refptr<base::SingleThreadTaskRunner>) override {
    return std::make_unique<Reader>(proxy_);
  }
  Persistent<MockReaderProxy> proxy_;
};

TEST_F(BytesConsumerForDataConsumerHandleTest, Create) {
  auto handle = std::make_unique<ReplayingWebDataConsumerHandle>();
  handle->Add(Command(Command::kData, "hello"));
  handle->Add(Command(Command::kDone));
  Persistent<BytesConsumer> consumer =
      MakeGarbageCollected<BytesConsumerForDataConsumerHandle>(
          GetTaskRunner(), std::move(handle));
}

TEST_F(BytesConsumerForDataConsumerHandleTest, BecomeReadable) {
  Checkpoint checkpoint;
  Persistent<MockBytesConsumerClient> client =
      MockBytesConsumerClient::Create();

  InSequence s;
  EXPECT_CALL(checkpoint, Call(1));
  EXPECT_CALL(*client, OnStateChange());
  EXPECT_CALL(checkpoint, Call(2));

  auto handle = std::make_unique<ReplayingWebDataConsumerHandle>();
  handle->Add(Command(Command::kData, "hello"));
  Persistent<BytesConsumer> consumer =
      MakeGarbageCollected<BytesConsumerForDataConsumerHandle>(
          GetTaskRunner(), std::move(handle));
  consumer->SetClient(client);
  EXPECT_EQ(BytesConsumer::PublicState::kReadableOrWaiting,
            consumer->GetPublicState());

  checkpoint.Call(1);
  platform_->RunUntilIdle();
  checkpoint.Call(2);
  EXPECT_EQ(BytesConsumer::PublicState::kReadableOrWaiting,
            consumer->GetPublicState());
}

TEST_F(BytesConsumerForDataConsumerHandleTest, BecomeClosed) {
  Checkpoint checkpoint;
  Persistent<MockBytesConsumerClient> client =
      MockBytesConsumerClient::Create();

  InSequence s;
  EXPECT_CALL(checkpoint, Call(1));
  EXPECT_CALL(*client, OnStateChange());
  EXPECT_CALL(checkpoint, Call(2));

  auto handle = std::make_unique<ReplayingWebDataConsumerHandle>();
  handle->Add(Command(Command::kDone));
  Persistent<BytesConsumer> consumer =
      MakeGarbageCollected<BytesConsumerForDataConsumerHandle>(
          GetTaskRunner(), std::move(handle));
  consumer->SetClient(client);
  EXPECT_EQ(BytesConsumer::PublicState::kReadableOrWaiting,
            consumer->GetPublicState());

  checkpoint.Call(1);
  platform_->RunUntilIdle();
  checkpoint.Call(2);
  EXPECT_EQ(BytesConsumer::PublicState::kClosed, consumer->GetPublicState());
}

TEST_F(BytesConsumerForDataConsumerHandleTest, BecomeErrored) {
  Checkpoint checkpoint;
  Persistent<MockBytesConsumerClient> client =
      MockBytesConsumerClient::Create();

  InSequence s;
  EXPECT_CALL(checkpoint, Call(1));
  EXPECT_CALL(*client, OnStateChange());
  EXPECT_CALL(checkpoint, Call(2));

  auto handle = std::make_unique<ReplayingWebDataConsumerHandle>();
  handle->Add(Command(Command::kError));
  Persistent<BytesConsumer> consumer =
      MakeGarbageCollected<BytesConsumerForDataConsumerHandle>(
          GetTaskRunner(), std::move(handle));
  consumer->SetClient(client);
  EXPECT_EQ(BytesConsumer::PublicState::kReadableOrWaiting,
            consumer->GetPublicState());

  checkpoint.Call(1);
  platform_->RunUntilIdle();
  checkpoint.Call(2);
  EXPECT_EQ(BytesConsumer::PublicState::kErrored, consumer->GetPublicState());
}

TEST_F(BytesConsumerForDataConsumerHandleTest, ClearClient) {
  Checkpoint checkpoint;
  Persistent<MockBytesConsumerClient> client =
      MockBytesConsumerClient::Create();

  InSequence s;
  EXPECT_CALL(checkpoint, Call(1));
  EXPECT_CALL(checkpoint, Call(2));

  auto handle = std::make_unique<ReplayingWebDataConsumerHandle>();
  handle->Add(Command(Command::kError));
  Persistent<BytesConsumer> consumer =
      MakeGarbageCollected<BytesConsumerForDataConsumerHandle>(
          GetTaskRunner(), std::move(handle));
  consumer->SetClient(client);
  consumer->ClearClient();

  checkpoint.Call(1);
  platform_->RunUntilIdle();
  checkpoint.Call(2);
}

TEST_F(BytesConsumerForDataConsumerHandleTest, TwoPhaseReadWhenReadable) {
  auto handle = std::make_unique<ReplayingWebDataConsumerHandle>();
  handle->Add(Command(Command::kData, "hello"));
  Persistent<BytesConsumer> consumer =
      MakeGarbageCollected<BytesConsumerForDataConsumerHandle>(
          GetTaskRunner(), std::move(handle));
  consumer->SetClient(MockBytesConsumerClient::Create());

  const char* buffer = nullptr;
  size_t available = 0;
  ASSERT_EQ(Result::kOk, consumer->BeginRead(&buffer, &available));
  EXPECT_EQ("hello", String(buffer, available));

  ASSERT_EQ(Result::kOk, consumer->EndRead(1));
  ASSERT_EQ(Result::kOk, consumer->BeginRead(&buffer, &available));
  EXPECT_EQ("ello", String(buffer, available));

  ASSERT_EQ(Result::kOk, consumer->EndRead(4));
  ASSERT_EQ(Result::kShouldWait, consumer->BeginRead(&buffer, &available));
}

TEST_F(BytesConsumerForDataConsumerHandleTest, TwoPhaseReadWhenWaiting) {
  auto handle = std::make_unique<ReplayingWebDataConsumerHandle>();
  Persistent<BytesConsumer> consumer =
      MakeGarbageCollected<BytesConsumerForDataConsumerHandle>(
          GetTaskRunner(), std::move(handle));
  consumer->SetClient(MockBytesConsumerClient::Create());
  const char* buffer = nullptr;
  size_t available = 0;
  ASSERT_EQ(Result::kShouldWait, consumer->BeginRead(&buffer, &available));
}

TEST_F(BytesConsumerForDataConsumerHandleTest, TwoPhaseReadWhenClosed) {
  auto handle = std::make_unique<ReplayingWebDataConsumerHandle>();
  handle->Add(Command(Command::kDone));
  Persistent<BytesConsumer> consumer =
      MakeGarbageCollected<BytesConsumerForDataConsumerHandle>(
          GetTaskRunner(), std::move(handle));
  consumer->SetClient(MockBytesConsumerClient::Create());
  const char* buffer = nullptr;
  size_t available = 0;
  ASSERT_EQ(Result::kDone, consumer->BeginRead(&buffer, &available));
}

TEST_F(BytesConsumerForDataConsumerHandleTest, TwoPhaseReadWhenErrored) {
  auto handle = std::make_unique<ReplayingWebDataConsumerHandle>();
  handle->Add(Command(Command::kError));
  Persistent<BytesConsumer> consumer =
      MakeGarbageCollected<BytesConsumerForDataConsumerHandle>(
          GetTaskRunner(), std::move(handle));
  consumer->SetClient(MockBytesConsumerClient::Create());
  const char* buffer = nullptr;
  size_t available = 0;
  ASSERT_EQ(Result::kError, consumer->BeginRead(&buffer, &available));
  EXPECT_EQ(BytesConsumer::Error("error"), consumer->GetError());
}

TEST_F(BytesConsumerForDataConsumerHandleTest, Cancel) {
  auto handle = std::make_unique<ReplayingWebDataConsumerHandle>();
  Persistent<BytesConsumer> consumer =
      MakeGarbageCollected<BytesConsumerForDataConsumerHandle>(
          GetTaskRunner(), std::move(handle));
  consumer->SetClient(MockBytesConsumerClient::Create());
  consumer->Cancel();
  const char* buffer = nullptr;
  size_t available = 0;
  ASSERT_EQ(Result::kDone, consumer->BeginRead(&buffer, &available));
}

TEST_F(BytesConsumerForDataConsumerHandleTest, drainAsBlobDataHandle) {
  // WebDataConsumerHandle::Reader::drainAsBlobDataHandle should return
  // nullptr from the second time, but we don't care that here.
  std::unique_ptr<MockDataConsumerHandle> handle =
      std::make_unique<MockDataConsumerHandle>();
  Persistent<MockDataConsumerHandle::MockReaderProxy> proxy = handle->Proxy();
  Persistent<BytesConsumer> consumer =
      MakeGarbageCollected<BytesConsumerForDataConsumerHandle>(
          GetTaskRunner(), std::move(handle));
  consumer->SetClient(MockBytesConsumerClient::Create());

  Checkpoint checkpoint;
  InSequence s;

  EXPECT_FALSE(consumer->DrainAsBlobDataHandle(
      BytesConsumer::BlobSizePolicy::kDisallowBlobWithInvalidSize));
  EXPECT_FALSE(consumer->DrainAsBlobDataHandle(
      BytesConsumer::BlobSizePolicy::kAllowBlobWithInvalidSize));
  EXPECT_EQ(BytesConsumer::PublicState::kReadableOrWaiting,
            consumer->GetPublicState());
}

TEST_F(BytesConsumerForDataConsumerHandleTest, drainAsFormData) {
  std::unique_ptr<MockDataConsumerHandle> handle =
      std::make_unique<MockDataConsumerHandle>();
  Persistent<MockDataConsumerHandle::MockReaderProxy> proxy = handle->Proxy();
  Persistent<BytesConsumer> consumer =
      MakeGarbageCollected<BytesConsumerForDataConsumerHandle>(
          GetTaskRunner(), std::move(handle));
  consumer->SetClient(MockBytesConsumerClient::Create());

  Checkpoint checkpoint;
  InSequence s;

  EXPECT_FALSE(consumer->DrainAsFormData());
  EXPECT_EQ(BytesConsumer::PublicState::kReadableOrWaiting,
            consumer->GetPublicState());
}

}  // namespace

}  // namespace blink
