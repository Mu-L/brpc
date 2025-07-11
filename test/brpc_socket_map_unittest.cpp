// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

// brpc - A framework to host and access services throughout Baidu.

// Date: Sun Jul 13 15:04:18 CST 2014

#include <gtest/gtest.h>
#include <gflags/gflags.h>
#include "brpc/socket.h"
#include "brpc/socket_map.h"
#include "brpc/reloadable_flags.h"

namespace brpc {
DECLARE_int32(health_check_interval);
DECLARE_int32(idle_timeout_second);
DECLARE_int32(defer_close_second);
DECLARE_int32(max_connection_pool_size);
} // namespace brpc

namespace {
butil::EndPoint g_endpoint;
brpc::SocketMapKey g_key(g_endpoint);

void* worker(void*) {
    const int ROUND = 2;
    const int COUNT = 1000;
    brpc::SocketId id;
    for (int i = 0; i < ROUND * 2; ++i) {
        for (int j = 0; j < COUNT; ++j) {
            if (i % 2 == 0) {
                EXPECT_EQ(0, brpc::SocketMapInsert(g_key, &id));
            } else {
                brpc::SocketMapRemove(g_key);
            }
        }
    }
    return NULL;
}

class SocketMapTest : public ::testing::Test{
protected:
    SocketMapTest(){};
    virtual ~SocketMapTest(){};
    virtual void SetUp(){};
    virtual void TearDown(){};
};

TEST_F(SocketMapTest, disable_health_check) {
    int32_t old_interval = brpc::FLAGS_health_check_interval;
    brpc::FLAGS_health_check_interval = 0;
    brpc::SocketId id;
    ASSERT_EQ(-1, brpc::SocketMapFind(g_key, &id));
    ASSERT_EQ(0, brpc::SocketMapInsert(g_key, &id));
    ASSERT_EQ(0, brpc::SocketMapFind(g_key, &id));
    {
        brpc::SocketUniquePtr ptr;
        ASSERT_EQ(0, brpc::Socket::Address(id, &ptr));
    }
    ASSERT_EQ(0, brpc::Socket::SetFailed(id));

    brpc::SocketUniquePtr ptr;
    // The socket should not be recycled,
    // because SocketMap holds a reference to it.
    ASSERT_EQ(1, brpc::Socket::AddressFailedAsWell(id, &ptr));
    ASSERT_EQ(2, ptr->nref());
    brpc::SocketMapRemove(g_key);
    // After removing the socket, `ptr' holds the last reference.
    ASSERT_EQ(1, ptr->nref());
    ASSERT_EQ(-1, brpc::SocketMapFind(g_key, &id));
    brpc::FLAGS_health_check_interval = old_interval;
}

TEST_F(SocketMapTest, idle_timeout) {
    const int TIMEOUT = 1;
    const int NTHREAD = 10;
    brpc::FLAGS_defer_close_second = TIMEOUT;
    pthread_t tids[NTHREAD];
    for (int i = 0; i < NTHREAD; ++i) {
        ASSERT_EQ(0, pthread_create(&tids[i], NULL, worker, NULL));
    }
    for (int i = 0; i < NTHREAD; ++i) {
        ASSERT_EQ(0, pthread_join(tids[i], NULL));
    }
    brpc::SocketId id;
    // Socket still exists since it has not reached timeout yet
    ASSERT_EQ(0, brpc::SocketMapFind(g_key, &id));
    usleep(TIMEOUT * 1000000L + 1100000L);
    // Socket should be removed after timeout
    ASSERT_EQ(-1, brpc::SocketMapFind(g_key, &id));

    brpc::FLAGS_defer_close_second = TIMEOUT * 10;
    ASSERT_EQ(0, brpc::SocketMapInsert(g_key, &id));
    brpc::SocketMapRemove(g_key);
    ASSERT_EQ(0, brpc::SocketMapFind(g_key, &id));
    // Change `FLAGS_idle_timeout_second' to 0 to disable checking
    brpc::FLAGS_defer_close_second = 0;
    usleep(1100000L);
    // And then Socket should be removed
    ASSERT_EQ(-1, brpc::SocketMapFind(g_key, &id));

    brpc::SocketId main_id;
    ASSERT_EQ(0, brpc::SocketMapInsert(g_key, &main_id));
    brpc::FLAGS_idle_timeout_second = TIMEOUT;
    brpc::SocketUniquePtr main_ptr;
    brpc::SocketUniquePtr ptr;
    ASSERT_EQ(0, brpc::Socket::Address(main_id, &main_ptr));
    ASSERT_EQ(0, main_ptr->GetPooledSocket(&ptr));
    ASSERT_TRUE(main_ptr.get());
    main_ptr.reset();
    id = ptr->id();
    ptr->ReturnToPool();
    ptr.reset(NULL);
    usleep(TIMEOUT * 1000000L + 2000000L);
    // Pooled connection should be `ReleaseAdditionalReference',
    // which destroyed the Socket. As a result `GetSocketFromPool'
    // should return a new one
    ASSERT_EQ(0, brpc::Socket::Address(main_id, &main_ptr));
    ASSERT_EQ(0, main_ptr->GetPooledSocket(&ptr));
    ASSERT_TRUE(main_ptr.get());
    main_ptr.reset();
    ASSERT_NE(id, ptr->id());
    brpc::SocketMapRemove(g_key);
}

TEST_F(SocketMapTest, max_pool_size) {
    const int MAXSIZE = 5;
    const int TOTALSIZE = MAXSIZE + 5;
    brpc::FLAGS_max_connection_pool_size = MAXSIZE;

    brpc::SocketId main_id;
    ASSERT_EQ(0, brpc::SocketMapInsert(g_key, &main_id));

    brpc::SocketUniquePtr ptrs[TOTALSIZE];
    for (int i = 0; i < TOTALSIZE; ++i) {
        brpc::SocketUniquePtr main_ptr;
        ASSERT_EQ(0, brpc::Socket::Address(main_id, &main_ptr));
        ASSERT_EQ(0, main_ptr->GetPooledSocket(&ptrs[i]));
        ASSERT_TRUE(main_ptr.get());
        main_ptr.reset();
    }
    for (int i = 0; i < TOTALSIZE; ++i) {
        ASSERT_EQ(0, ptrs[i]->ReturnToPool());
    }
    std::vector<brpc::SocketId> ids;
    brpc::SocketUniquePtr main_ptr;
    ASSERT_EQ(0, brpc::Socket::Address(main_id, &main_ptr));
    main_ptr->ListPooledSockets(&ids);
    EXPECT_EQ(MAXSIZE, (int)ids.size());
    // The last few Sockets should be `SetFailed' by `ReturnSocketToPool'
    for (int i = MAXSIZE; i < TOTALSIZE; ++i) {
        EXPECT_TRUE(ptrs[i]->Failed());
    }
}

} //namespace

int main(int argc, char* argv[]) {
    butil::str2endpoint("127.0.0.1:12345", &g_key.peer.addr);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
