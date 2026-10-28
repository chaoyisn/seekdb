/*
 * Copyright (c) 2025 OceanBase.
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

#define USING_LOG_PREFIX STORAGE_FTS

#include "storage/fts/dict/ob_ft_dict_hub.h"

#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/utility/ob_macro_utils.h"
#include "storage/fts/dict/ob_ft_cache_container.h"
#include "storage/fts/dict/ob_ft_dict_def.h"
#include "storage/fts/dict/ob_ft_range_dict.h"
namespace oceanbase
{
namespace storage
{
int ObFTDictHub::init()
{
  static constexpr int K_MAX_DICT_BUCKET = 128; // for now, only built-in dicts.
  int ret = OB_SUCCESS;
  if (OB_FAIL(dict_map_.create(K_MAX_DICT_BUCKET, "dict_map"))) {
  } else if (OB_FAIL(rw_dict_lock_.init(K_MAX_DICT_BUCKET))) {
  } else {
    is_inited_ = true;
  }
  return ret;
};

int ObFTDictHub::destroy()
{
  clear_ik_dicts();
  is_inited_ = false;
  return OB_SUCCESS;
}

void ObFTDictHub::clear_ik_dicts()
{
  ATOMIC_STORE(&ik_dict_ready_, false);
  if (OB_NOT_NULL(ik_dict_main_)) {
    OB_DELETEx(ObFTRangeDict, &ik_dict_allocator_, ik_dict_main_);
    ik_dict_main_ = nullptr;
  }
  if (OB_NOT_NULL(ik_dict_quan_)) {
    OB_DELETEx(ObFTRangeDict, &ik_dict_allocator_, ik_dict_quan_);
    ik_dict_quan_ = nullptr;
  }
  if (OB_NOT_NULL(ik_dict_stop_)) {
    OB_DELETEx(ObFTRangeDict, &ik_dict_allocator_, ik_dict_stop_);
    ik_dict_stop_ = nullptr;
  }
  if (OB_NOT_NULL(ik_main_container_)) {
    OB_DELETEx(ObFTCacheRangeContainer, &ik_dict_allocator_, ik_main_container_);
    ik_main_container_ = nullptr;
  }
  if (OB_NOT_NULL(ik_quan_container_)) {
    OB_DELETEx(ObFTCacheRangeContainer, &ik_dict_allocator_, ik_quan_container_);
    ik_quan_container_ = nullptr;
  }
  if (OB_NOT_NULL(ik_stop_container_)) {
    OB_DELETEx(ObFTCacheRangeContainer, &ik_dict_allocator_, ik_stop_container_);
    ik_stop_container_ = nullptr;
  }
  ik_dict_allocator_.reset();
}
int ObFTDictHub::build_cache(const ObFTDictDesc &desc, ObFTCacheRangeContainer &container)
{
  int ret = OB_SUCCESS;
  ObFTDictInfoKey key(static_cast<uint64_t>(desc.type_));
  ObFTDictInfo info;
  container.reset();

  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else {
    ObBucketHashWLockGuard guard(rw_dict_lock_, key.hash());

    // try if valid with no recursive lock
    if (OB_FAIL(get_dict_info(key, info))) {
      if (OB_HASH_NOT_EXIST == ret) {
        // dict not exist, make new one, by caller
        ret = OB_ENTRY_NOT_EXIST;
      } else {
      }
    } else if (OB_FAIL(ObFTRangeDict::try_load_cache(desc, info.range_count_, container))) {
      if (OB_ENTRY_NOT_EXIST == ret) {
      } else {
      }
    }

    if (OB_FAIL(ret)) {
      if (OB_ENTRY_NOT_EXIST == ret) {
        if (OB_FAIL(ObFTRangeDict::build_cache_from_ik_dict(desc, container))) {
        } else if (FALSE_IT(info.range_count_ = container.get_handles().size())) {
        } else if (OB_FAIL(put_dict_info(key, info))) {
        }
      }
    }
  }
  return ret;
}

int ObFTDictHub::load_cache(const ObFTDictDesc &desc, ObFTCacheRangeContainer &container)
{
  int ret = OB_SUCCESS;
  ObFTDictInfo info;
  container.reset();
  ObFTDictInfoKey key(static_cast<uint64_t>(desc.type_));
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else {
    {
      ObBucketHashRLockGuard guard(rw_dict_lock_, key.hash());
      if (OB_FAIL(get_dict_info(key, info))) {
        if (OB_HASH_NOT_EXIST == ret) {
          // dict not exist, make new one, by caller
          ret = OB_ENTRY_NOT_EXIST;
        } else {
        }
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObFTRangeDict::try_load_cache(desc, info.range_count_, container))) {
      if (OB_ENTRY_NOT_EXIST == ret) {
        // dict not exist, make new one, by caller
      } else {
      }
    }
  }

  return ret;
}


int ObFTDictHub::get_ik_dicts(ObIFTDict *&dict_main, ObIFTDict *&dict_quan, ObIFTDict *&dict_stop)
{
  int ret = OB_SUCCESS;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else if (OB_LIKELY(ATOMIC_LOAD(&ik_dict_ready_))) {
    dict_main = ik_dict_main_;
    dict_quan = ik_dict_quan_;
    dict_stop = ik_dict_stop_;
  } else {
    common::SpinWLockGuard guard(ik_dict_lock_);
    if (ATOMIC_LOAD(&ik_dict_ready_)) {
      dict_main = ik_dict_main_;
      dict_quan = ik_dict_quan_;
      dict_stop = ik_dict_stop_;
    } else {
      ObFTDictDesc main_desc("main_dict",
                             ObFTDictType::DICT_IK_MAIN,
                             ObCharsetType::CHARSET_UTF8MB4,
                             ObCollationType::CS_TYPE_UTF8MB4_BIN);
      ObFTDictDesc quan_desc("quan_dict",
                             ObFTDictType::DICT_IK_QUAN,
                             ObCharsetType::CHARSET_UTF8MB4,
                             ObCollationType::CS_TYPE_UTF8MB4_BIN);
      ObFTDictDesc stop_desc("stopword",
                             ObFTDictType::DICT_IK_STOP,
                             ObCharsetType::CHARSET_UTF8MB4,
                             ObCollationType::CS_TYPE_UTF8MB4_BIN);

      if (OB_ISNULL(ik_main_container_ = OB_NEWx(ObFTCacheRangeContainer, &ik_dict_allocator_, ik_dict_allocator_))
          || OB_ISNULL(ik_quan_container_ = OB_NEWx(ObFTCacheRangeContainer, &ik_dict_allocator_, ik_dict_allocator_))
          || OB_ISNULL(ik_stop_container_ = OB_NEWx(ObFTCacheRangeContainer, &ik_dict_allocator_, ik_dict_allocator_))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        auto load_or_build = [this](const ObFTDictDesc &desc, ObFTCacheRangeContainer &c) -> int {
          int r = this->load_cache(desc, c);
          if (OB_ENTRY_NOT_EXIST == r) {
            r = this->build_cache(desc, c);
          }
          return r;
        };

        if (OB_FAIL(load_or_build(main_desc, *ik_main_container_))) {
        } else if (OB_FAIL(load_or_build(quan_desc, *ik_quan_container_))) {
        } else if (OB_FAIL(load_or_build(stop_desc, *ik_stop_container_))) {
        } else {
          ik_dict_main_ = OB_NEWx(ObFTRangeDict, &ik_dict_allocator_, ik_dict_allocator_, ik_main_container_, main_desc);
          ik_dict_quan_ = OB_NEWx(ObFTRangeDict, &ik_dict_allocator_, ik_dict_allocator_, ik_quan_container_, quan_desc);
          ik_dict_stop_ = OB_NEWx(ObFTRangeDict, &ik_dict_allocator_, ik_dict_allocator_, ik_stop_container_, stop_desc);
          if (OB_ISNULL(ik_dict_main_) || OB_ISNULL(ik_dict_quan_) || OB_ISNULL(ik_dict_stop_)) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else if (OB_FAIL(ik_dict_main_->init()) || OB_FAIL(ik_dict_quan_->init()) || OB_FAIL(ik_dict_stop_->init())) {
          } else {
            dict_main = ik_dict_main_;
            dict_quan = ik_dict_quan_;
            dict_stop = ik_dict_stop_;
            ATOMIC_STORE(&ik_dict_ready_, true);
          }
        }
      }
      if (OB_FAIL(ret)) {
        clear_ik_dicts();
      }
    }
  }
  return ret;
}

int ObFTDictHub::get_dict_info(const ObFTDictInfoKey &key, ObFTDictInfo &info)
{
  int ret = OB_SUCCESS;

  if (OB_FAIL(dict_map_.get_refactored(key, info))) {
    if (OB_HASH_NOT_EXIST != ret) {
    }
  }

  return ret;
}

int ObFTDictHub::put_dict_info(const ObFTDictInfoKey &key, const ObFTDictInfo &info)
{
  int ret = OB_SUCCESS;
  const int cover_exist_flag = 1;
  if (OB_FAIL(dict_map_.set_refactored(key, info, cover_exist_flag))) {
  }

  return ret;
}
} //  namespace storage
} //  namespace oceanbase
