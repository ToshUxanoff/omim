#include "indexer/mwm_set.hpp"
#include "indexer/scales.hpp"

#include "defines.hpp"

#include "base/assert.hpp"
#include "base/logging.hpp"
#include "base/stl_add.hpp"

#include "std/algorithm.hpp"

using platform::CountryFile;
using platform::LocalCountryFile;

MwmInfo::MwmInfo() : m_minScale(0), m_maxScale(0), m_status(STATUS_DEREGISTERED), m_lockCount(0) {}

MwmInfo::MwmTypeT MwmInfo::GetType() const
{
  if (m_minScale > 0)
    return COUNTRY;
  if (m_maxScale == scales::GetUpperWorldScale())
    return WORLD;
  ASSERT_EQUAL(m_maxScale, scales::GetUpperScale(), ());
  return COASTS;
}

MwmSet::MwmLock::MwmLock() : m_mwmSet(nullptr), m_mwmId(), m_value(nullptr) {}

MwmSet::MwmLock::MwmLock(MwmSet & mwmSet, MwmId const & mwmId)
    : m_mwmSet(&mwmSet), m_mwmId(mwmId), m_value(m_mwmSet->LockValue(m_mwmId))
{
}

MwmSet::MwmLock::MwmLock(MwmSet & mwmSet, MwmId const & mwmId, TMwmValueBasePtr value)
    : m_mwmSet(&mwmSet), m_mwmId(mwmId), m_value(value)
{
}

MwmSet::MwmLock::MwmLock(MwmLock && lock)
    : m_mwmSet(lock.m_mwmSet), m_mwmId(lock.m_mwmId), m_value(move(lock.m_value))
{
  lock.m_mwmSet = nullptr;
  lock.m_mwmId.Reset();
}

MwmSet::MwmLock::~MwmLock()
{
  if (m_mwmSet && m_value)
    m_mwmSet->UnlockValue(m_mwmId, m_value);
}

shared_ptr<MwmInfo> const & MwmSet::MwmLock::GetInfo() const
{
  ASSERT(IsLocked(), ("MwmLock is not active."));
  return m_mwmId.GetInfo();
}

MwmSet::MwmLock & MwmSet::MwmLock::operator=(MwmLock && lock)
{
  swap(m_mwmSet, lock.m_mwmSet);
  swap(m_mwmId, lock.m_mwmId);
  swap(m_value, lock.m_value);
  return *this;
}

MwmSet::MwmSet(size_t cacheSize)
  : m_cacheSize(cacheSize)
{
}

MwmSet::~MwmSet()
{
  // Need do call Cleanup() in derived class.
  ASSERT(m_cache.empty(), ());
}

void MwmSet::Cleanup()
{
  lock_guard<mutex> lock(m_lock);
  ClearCacheImpl(m_cache.begin(), m_cache.end());
}

MwmSet::MwmId MwmSet::GetMwmIdByCountryFileImpl(CountryFile const & countryFile) const
{
  string const name = countryFile.GetNameWithoutExt();
  ASSERT(!name.empty(), ());
  auto const it = m_info.find(name);
  if (it == m_info.cend() || it->second.empty())
    return MwmId();
  return MwmId(it->second.back());
}

pair<MwmSet::MwmLock, bool> MwmSet::Register(LocalCountryFile const & localFile)
{
  lock_guard<mutex> lock(m_lock);

  CountryFile const & countryFile = localFile.GetCountryFile();
  string const name = countryFile.GetNameWithoutExt();

  MwmId const id = GetMwmIdByCountryFileImpl(countryFile);
  if (!id.IsAlive())
    return RegisterImpl(localFile);

  shared_ptr<MwmInfo> info = id.GetInfo();

  // Deregister old mwm for the country.
  if (info->GetVersion() < localFile.GetVersion())
  {
    DeregisterImpl(id);
    return RegisterImpl(localFile);
  }

  // Update the status of the mwm with the same version.
  if (info->GetVersion() == localFile.GetVersion())
  {
    LOG(LWARNING, ("Trying to add already registered mwm:", name));
    info->SetStatus(MwmInfo::STATUS_REGISTERED);
    return make_pair(GetLock(id), false);
  }

  LOG(LWARNING, ("Trying to add too old (", localFile.GetVersion(), ") mwm (", name,
                 "), current version:", info->GetVersion()));
  return make_pair(MwmLock(), false);
}

pair<MwmSet::MwmLock, bool> MwmSet::RegisterImpl(LocalCountryFile const & localFile)
{
  shared_ptr<MwmInfo> info(new MwmInfo());

  // This function can throw an exception for a bad mwm file.
  if (!GetVersion(localFile, *info))
    return make_pair(MwmLock(), false);
  info->SetStatus(MwmInfo::STATUS_REGISTERED);
  info->m_file = localFile;
  string const name = localFile.GetCountryFile().GetNameWithoutExt();

  vector<shared_ptr<MwmInfo>> & infos = m_info[name];
  infos.push_back(info);
  return make_pair(GetLock(MwmId(info)), true);
}

bool MwmSet::DeregisterImpl(MwmId const & id)
{
  if (!id.IsAlive())
    return false;
  shared_ptr<MwmInfo> const & info = id.GetInfo();
  string const name = info->GetCountryName();

  if (info->m_lockCount == 0)
  {
    info->SetStatus(MwmInfo::STATUS_DEREGISTERED);
    vector<shared_ptr<MwmInfo>> & infos = m_info[name];
    infos.erase(remove(infos.begin(), infos.end(), info), infos.end());
    OnMwmDeregistered(info->GetLocalFile());
    return true;
  }
  info->SetStatus(MwmInfo::STATUS_MARKED_TO_DEREGISTER);
  return false;
}

bool MwmSet::Deregister(CountryFile const & countryFile)
{
  lock_guard<mutex> lock(m_lock);
  return DeregisterImpl(countryFile);
}

bool MwmSet::DeregisterImpl(CountryFile const & countryFile)
{
  MwmId const id = GetMwmIdByCountryFileImpl(countryFile);
  if (!id.IsAlive())
    return false;
  bool const deregistered = DeregisterImpl(id);
  ClearCache(id);
  return deregistered;
}

void MwmSet::DeregisterAll()
{
  lock_guard<mutex> lock(m_lock);

  for (auto const & p : m_info)
  {
    // Vector of shared pointers is copied here because an original
    // vector will be modified by the body of the cycle.
    vector<shared_ptr<MwmInfo>> infos = p.second;
    for (shared_ptr<MwmInfo> info : infos)
      DeregisterImpl(MwmId(info));
  }

  // Do not call ClearCache - it's under mutex lock.
  ClearCacheImpl(m_cache.begin(), m_cache.end());
}

bool MwmSet::IsLoaded(CountryFile const & countryFile) const
{
  lock_guard<mutex> lock(m_lock);

  MwmId const id = GetMwmIdByCountryFileImpl(countryFile);
  return id.IsAlive() && id.GetInfo()->IsRegistered();
}

void MwmSet::GetMwmsInfo(vector<shared_ptr<MwmInfo>> & info) const
{
  lock_guard<mutex> lock(m_lock);
  info.clear();
  info.reserve(m_info.size());
  for (auto const & p : m_info)
  {
    if (!p.second.empty())
      info.push_back(p.second.back());
  }
}

MwmSet::TMwmValueBasePtr MwmSet::LockValue(MwmId const & id)
{
  lock_guard<mutex> lock(m_lock);
  return LockValueImpl(id);
}

MwmSet::TMwmValueBasePtr MwmSet::LockValueImpl(MwmId const & id)
{
  CHECK(id.IsAlive(), (id));
  shared_ptr<MwmInfo> info = id.GetInfo();
  if (!info->IsUpToDate())
    return TMwmValueBasePtr();

  ++info->m_lockCount;

  // Search in cache.
  for (CacheType::iterator it = m_cache.begin(); it != m_cache.end(); ++it)
  {
    if (it->first == id)
    {
      TMwmValueBasePtr result = it->second;
      m_cache.erase(it);
      return result;
    }
  }
  return CreateValue(info->GetLocalFile());
}

void MwmSet::UnlockValue(MwmId const & id, TMwmValueBasePtr p)
{
  lock_guard<mutex> lock(m_lock);
  UnlockValueImpl(id, p);
}

void MwmSet::UnlockValueImpl(MwmId const & id, TMwmValueBasePtr p)
{
  ASSERT(id.IsAlive(), ());
  ASSERT(p.get() != nullptr, (id));
  if (!id.IsAlive() || p.get() == nullptr)
    return;

  shared_ptr<MwmInfo> const & info = id.GetInfo();
  CHECK_GREATER(info->m_lockCount, 0, ());
  --info->m_lockCount;
  if (info->m_lockCount == 0 && info->GetStatus() == MwmInfo::STATUS_MARKED_TO_DEREGISTER)
    CHECK(DeregisterImpl(id), ());

  if (info->IsUpToDate())
  {
    m_cache.push_back(make_pair(id, p));
    if (m_cache.size() > m_cacheSize)
    {
      ASSERT_EQUAL(m_cache.size(), m_cacheSize + 1, ());
      m_cache.pop_front();
    }
  }
}

void MwmSet::ClearCache()
{
  lock_guard<mutex> lock(m_lock);
  ClearCacheImpl(m_cache.begin(), m_cache.end());
}

MwmSet::MwmId MwmSet::GetMwmIdByCountryFile(CountryFile const & countryFile) const
{
  lock_guard<mutex> lock(m_lock);

  MwmId const id = GetMwmIdByCountryFileImpl(countryFile);
  ASSERT(id.IsAlive(), ("Can't get an mwm's (", countryFile.GetNameWithoutExt(), ") identifier."));
  return id;
}

MwmSet::MwmLock MwmSet::GetMwmLockByCountryFile(CountryFile const & countryFile)
{
  lock_guard<mutex> lock(m_lock);

  MwmId const id = GetMwmIdByCountryFileImpl(countryFile);
  TMwmValueBasePtr value(nullptr);
  if (id.IsAlive())
    value = LockValueImpl(id);
  return MwmLock(*this, id, value);
}

void MwmSet::ClearCacheImpl(CacheType::iterator beg, CacheType::iterator end)
{
  m_cache.erase(beg, end);
}

namespace
{
  struct MwmIdIsEqualTo
  {
    MwmSet::MwmId m_id;

    explicit MwmIdIsEqualTo(MwmSet::MwmId const & id) : m_id(id) {}

    bool operator()(pair<MwmSet::MwmId, MwmSet::TMwmValueBasePtr> const & p) const
    {
      return p.first == m_id;
    }
  };
}

void MwmSet::ClearCache(MwmId const & id)
{
  ClearCacheImpl(RemoveIfKeepValid(m_cache.begin(), m_cache.end(), MwmIdIsEqualTo(id)), m_cache.end());
}
