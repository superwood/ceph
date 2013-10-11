// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
#include "gtest/gtest.h"

#include "mds/mdstypes.h"
#include "include/buffer.h"
#include "include/rbd_types.h"
#include "include/rados/librados.h"
#include "include/rados/librados.hpp"
#include "include/stringify.h"
#include "include/types.h"
#include "global/global_context.h"
#include "global/global_init.h"
#include "common/ceph_argparse.h"
#include "common/common_init.h"
#include "test/librados/test.h"

#include "osd/HitSet.h"

#include <errno.h>
#include <map>
#include <sstream>
#include <string>

using namespace librados;
using ceph::buffer;
using std::map;
using std::ostringstream;
using std::string;

TEST(LibRadosMisc, Dirty) {
  Rados cluster;
  std::string pool_name = get_temp_pool_name();
  ASSERT_EQ("", create_one_pool_pp(pool_name, cluster));
  IoCtx ioctx;
  ASSERT_EQ(0, cluster.ioctx_create(pool_name.c_str(), ioctx));

  {
    ObjectWriteOperation op;
    op.create(true);
    ASSERT_EQ(0, ioctx.operate("foo", &op));
  }
  {
    bool dirty = false;
    int r = -1;
    ObjectReadOperation op;
    op.is_dirty(&dirty, &r);
    ASSERT_EQ(0, ioctx.operate("foo", &op, NULL));
    ASSERT_TRUE(dirty);
    ASSERT_EQ(0, r);
  }
  {
    ObjectWriteOperation op;
    op.undirty();
    ASSERT_EQ(0, ioctx.operate("foo", &op));
  }
  {
    bool dirty = false;
    int r = -1;
    ObjectReadOperation op;
    op.is_dirty(&dirty, &r);
    ASSERT_EQ(0, ioctx.operate("foo", &op, NULL));
    ASSERT_FALSE(dirty);
    ASSERT_EQ(0, r);
  }
  {
    ObjectWriteOperation op;
    op.truncate(0);  // still a write even tho it is a no-op
    ASSERT_EQ(0, ioctx.operate("foo", &op));
  }
  {
    bool dirty = false;
    int r = -1;
    ObjectReadOperation op;
    op.is_dirty(&dirty, &r);
    ASSERT_EQ(0, ioctx.operate("foo", &op, NULL));
    ASSERT_TRUE(dirty);
    ASSERT_EQ(0, r);
  }

  ioctx.close();
  ASSERT_EQ(0, destroy_one_pool_pp(pool_name, cluster));
}

TEST(LibRadosMisc, HitSetNone) {
  Rados cluster;
  std::string pool_name = get_temp_pool_name();
  ASSERT_EQ("", create_one_pool_pp(pool_name, cluster));
  IoCtx ioctx;
  ASSERT_EQ(0, cluster.ioctx_create(pool_name.c_str(), ioctx));

  {
    list< pair<time_t,time_t> > ls;
    AioCompletion *c = librados::Rados::aio_create_completion();
    ASSERT_EQ(0, ioctx.hit_set_list(123, c, &ls));
    c->wait_for_complete();
    ASSERT_EQ(0, c->get_return_value());
    ASSERT_TRUE(ls.empty());
    c->release();
  }
  {
    bufferlist bl;
    AioCompletion *c = librados::Rados::aio_create_completion();
    ASSERT_EQ(0, ioctx.hit_set_get(123, c, 12345, &bl));
    c->wait_for_complete();
    ASSERT_EQ(-ENOENT, c->get_return_value());
    c->release();
  }

  ioctx.close();
  ASSERT_EQ(0, destroy_one_pool_pp(pool_name, cluster));
}

string set_pool_str(string pool, string var, string val)
{
  return string("{\"prefix\": \"osd pool set\",\"pool\":\"") + pool
    + string("\",\"var\": \"") + var + string("\",\"val\": \"")
    + val + string("\"}");
}

string set_pool_str(string pool, string var, int val)
{
  return string("{\"prefix\": \"osd pool set\",\"pool\":\"") + pool
    + string("\",\"var\": \"") + var + string("\",\"val\": ")
    + stringify(val) + string("}");
}

TEST(LibRadosMisc, HitSetRead) {
  Rados cluster;
  std::string pool_name = get_temp_pool_name();
  ASSERT_EQ("", create_one_pool_pp(pool_name, cluster));
  IoCtx ioctx;
  ASSERT_EQ(0, cluster.ioctx_create(pool_name.c_str(), ioctx));

  // FIXME: detect num pgs
  int num_pg = 8;

  // enable hitset tracking for this pool
  bufferlist inbl;
  ASSERT_EQ(0, cluster.mon_command(set_pool_str(pool_name, "hit_set_count", 8),
						inbl, NULL, NULL));
  ASSERT_EQ(0, cluster.mon_command(set_pool_str(pool_name, "hit_set_period", 60),
						inbl, NULL, NULL));
  ASSERT_EQ(0, cluster.mon_command(set_pool_str(pool_name, "hit_set_type", "bloom"),
				   inbl, NULL, NULL));
  ASSERT_EQ(0, cluster.mon_command(set_pool_str(pool_name, "hit_set_fpp", ".01"),
				   inbl, NULL, NULL));

  // do a bunch of reads
  for (int i=0; i<1000; ++i) {
    bufferlist bl;
    ASSERT_EQ(-ENOENT, ioctx.read(stringify(i), bl, 1, 0));
  }

  // get HitSets
  std::map<int,HitSet> hitsets;
  for (int i=0; i<num_pg; ++i) {
    list< pair<time_t,time_t> > ls;
    AioCompletion *c = librados::Rados::aio_create_completion();
    ASSERT_EQ(0, ioctx.hit_set_list(i, c, &ls));
    c->wait_for_complete();
    c->release();
    std::cout << "pg " << i << " ls " << ls << std::endl;
    ASSERT_FALSE(ls.empty());

    // get the latest
    c = librados::Rados::aio_create_completion();
    bufferlist bl;
    ASSERT_EQ(0, ioctx.hit_set_get(i, c, ls.back().first, &bl));
    c->wait_for_complete();
    c->release();

    //std::cout << "bl len is " << bl.length() << "\n";
    //bl.hexdump(std::cout);
    //std::cout << std::endl;

    bufferlist::iterator p = bl.begin();
    ::decode(hitsets[i], p);
  }

  for (int i=0; i<1000; ++i) {
    string n = stringify(i);
    uint32_t hash = ioctx.get_object_hash_position(n);
    hobject_t oid(sobject_t(n, CEPH_NOSNAP), "", hash, -1, "");
    std::cout << "checking for " << oid << std::endl;
    bool found = false;
    for (int p=0; p<num_pg; ++p) {
      if (hitsets[p].contains(oid)) {
	found = true;
	break;
      }
    }
    ASSERT_TRUE(found);
  }

  ioctx.close();
  ASSERT_EQ(0, destroy_one_pool_pp(pool_name, cluster));
}


int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);

  vector<const char*> args;
  argv_to_vec(argc, (const char **)argv, args);

  global_init(NULL, args, CEPH_ENTITY_TYPE_CLIENT, CODE_ENVIRONMENT_UTILITY, 0);
  common_init_finish(g_ceph_context);

  return RUN_ALL_TESTS();
}
