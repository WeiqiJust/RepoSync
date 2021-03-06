/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/**
 * Copyright (c) 2014,  Regents of the University of California.
 *
 * This file is part of NDN repo-ng (Next generation of NDN repository).
 * See AUTHORS.md for complete list of repo-ng authors and contributors.
 *
 * repo-ng is free software: you can redistribute it and/or modify it under the terms
 * of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * repo-ng is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * repo-ng, e.g., in COPYING.md file.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "index.hpp"
#include "skiplist.hpp"

#include <ndn-cxx/util/crypto.hpp>
#include "ndn-cxx/security/signature-sha256-with-rsa.hpp"

namespace repo {

/** @brief determines if entry can satisfy interest
 *  @param hash SHA256 hash of PublisherPublicKeyLocator if exists in interest, otherwise ignored
 */
static bool
matchesSimpleSelectors(const Interest& interest, ndn::ConstBufferPtr& hash,
                       const Index::Entry& entry)
{
  if (entry.getStatus() == DELETED)
    return false;
  const Name& fullName = entry.getName();

  if (!interest.getName().isPrefixOf(fullName))
    return false;

  size_t nSuffixComponents = fullName.size() - interest.getName().size();
  if (interest.getMinSuffixComponents() >= 0 &&
      nSuffixComponents < static_cast<size_t>(interest.getMinSuffixComponents()))
    return false;
  if (interest.getMaxSuffixComponents() >= 0 &&
      nSuffixComponents > static_cast<size_t>(interest.getMaxSuffixComponents()))
    return false;

  if (!interest.getExclude().empty() &&
      entry.getName().size() > interest.getName().size() &&
      interest.getExclude().isExcluded(entry.getName()[interest.getName().size()]))
    return false;
  if (!interest.getPublisherPublicKeyLocator().empty())
    {
      if (*entry.getKeyLocatorHash() != *hash)
          return false;
    }
  return true;
}

Index::Index(const size_t nMaxPackets)
  : m_maxPackets(nMaxPackets)
  , m_size(0)
{
}

void
Index::entryEnumeration(ndn::function< void (const Name &, const status &) > f) const
{
  for (IndexSkipList::const_iterator iter = m_skipList.begin(); iter != m_skipList.end(); ++iter)
  {
    f(iter->getName(), iter->getStatus());
  }
}

bool
Index::insert(const Data& data, const int64_t id)
{
  if (isFull())
    throw Error("The Index is Full. Cannot Insert Any Data!");
  Entry entry(data, id);
  IndexSkipList::const_iterator result = m_skipList.find(entry);
  bool isInserted = false;
  if (result == m_skipList.end()) {
    isInserted = m_skipList.insert(entry).second;
  }
  else if (result->getStatus() == DELETED) {
    m_skipList.erase(result);
    entry.setStatus(INSERTED);
    isInserted = m_skipList.insert(entry).second;
  }
  if (isInserted)
    ++m_size;
  return isInserted;
}

bool
Index::insert(const Name& fullName, const int64_t id,
              const ndn::ConstBufferPtr& keyLocatorHash)
{
  if (isFull())
    throw Error("The Index is Full. Cannot Insert Any Data!");
  Entry entry(fullName, keyLocatorHash, id);
  IndexSkipList::const_iterator result = m_skipList.find(entry);
  bool isInserted = false;
  if (result == m_skipList.end()) {
    isInserted = m_skipList.insert(entry).second;
  }
  else if (result->getStatus() == DELETED) {
    m_skipList.erase(result);
    entry.setStatus(INSERTED);
    isInserted = m_skipList.insert(entry).second;
  }
  if (isInserted)
    ++m_size;
  return isInserted;
}

std::pair<int64_t,Name>
Index::find(const Interest& interest) const
{
  Name name = interest.getName();
  IndexSkipList::const_iterator result = m_skipList.lower_bound(name);
  if (result != m_skipList.end())
    {
      return selectChild(interest, result);
    }
  else
    {
      return std::make_pair(0, Name());
    }
}

std::pair<int64_t,Name>
Index::find(const Name& name) const
{
  IndexSkipList::const_iterator result = m_skipList.lower_bound(name);
  if (result != m_skipList.end())
    {
      return findFirstEntry(name, result);
    }
  else
    {
      return std::make_pair(0, Name());
    }
}

status
Index::getStatus(const Name& name) const
{
  IndexSkipList::const_iterator result = m_skipList.lower_bound(name);
  if (result == m_skipList.end())
    return NONE;
  if (name.isPrefixOf(result->getName()))
    return result->getStatus();
  else
    return NONE;
}

bool
Index::hasData(const Data& data) const
{
  Index::Entry entry(data, -1); // the id number is useless
  IndexSkipList::const_iterator result = m_skipList.find(entry);
  return result != m_skipList.end() && result->getStatus() != DELETED;
}

std::pair<int64_t,Name>
Index::findFirstEntry(const Name& prefix,
                      IndexSkipList::const_iterator startingPoint) const
{
  BOOST_ASSERT(startingPoint != m_skipList.end());
  for (IndexSkipList::const_iterator iter = startingPoint; iter != m_skipList.end(); iter++) {
    if (iter->getStatus() == DELETED)
      continue;
    if (prefix.isPrefixOf(iter->getName()))
    {
      return std::make_pair(iter->getId(), iter->getName());
    }
    else
    {
      return std::make_pair(0, Name());
    }
  }
  return std::make_pair(0, Name());
}

bool
Index::erase(const Name& fullName)
{
  Entry entry(fullName);
  IndexSkipList::const_iterator findIterator = m_skipList.find(entry);
  if (findIterator != m_skipList.end())
    {
      Entry remove(*findIterator);
      remove.setStatus(DELETED);
      m_skipList.erase(findIterator);
      if (!m_skipList.insert(remove).second)
        throw Error("Delete Entry: Cannot change status!");
      m_size--;
      return true;
    }
  else
    return false;
}

void
Index::removeDeletedEntry()
{
  IndexSkipList::const_iterator iter = m_skipList.begin();
  while(iter != m_skipList.end()) {
    if (iter->getStatus() == DELETED) {
      iter = m_skipList.erase(iter);
      continue;
    }
    iter++;
  }
}

const ndn::ConstBufferPtr
Index::computeKeyLocatorHash(const KeyLocator& keyLocator)
{
  const Block& block = keyLocator.wireEncode();
  ndn::ConstBufferPtr keyLocatorHash = ndn::crypto::sha256(block.wire(), block.size());
  return keyLocatorHash;
}

std::pair<int64_t,Name>
Index::selectChild(const Interest& interest,
                   IndexSkipList::const_iterator startingPoint) const
{
  BOOST_ASSERT(startingPoint != m_skipList.end());
  bool isLeftmost = (interest.getChildSelector() <= 0);
  ndn::ConstBufferPtr hash;
  if (!interest.getPublisherPublicKeyLocator().empty())
    {
      KeyLocator keyLocator = interest.getPublisherPublicKeyLocator();
      const Block& block = keyLocator.wireEncode();
      hash = ndn::crypto::sha256(block.wire(), block.size());
    }

  if (isLeftmost)
    {
      for (IndexSkipList::const_iterator it = startingPoint;
           it != m_skipList.end(); ++it)
        {
          if (it->getStatus() == DELETED)
            continue;
          if (!interest.getName().isPrefixOf(it->getName()))
            return std::make_pair(0, Name());
          if (matchesSimpleSelectors(interest, hash, (*it)))
            return std::make_pair(it->getId(), it->getName());
        }
    }
  else
    {
      IndexSkipList::const_iterator boundary = m_skipList.lower_bound(interest.getName());
      while (boundary != m_skipList.end() && boundary->getStatus() == DELETED)
        boundary++;
      if (boundary == m_skipList.end() || !interest.getName().isPrefixOf(boundary->getName()))
        return std::make_pair(0, Name());
      Name successor = interest.getName().getSuccessor();
      IndexSkipList::const_iterator last = interest.getName().size() == 0 ?
                    m_skipList.end() : m_skipList.lower_bound(interest.getName().getSuccessor());
      while (last != m_skipList.end() && last->getStatus() == DELETED)
        last++;
      while (true)
        {
          IndexSkipList::const_iterator prev = last;
          --prev;
          if (prev == boundary && prev->getStatus() != DELETED)
            {
              bool isMatch = matchesSimpleSelectors(interest, hash, (*prev));
              if (isMatch)
                {
                  return std::make_pair(prev->getId(), prev->getName());
                }
              else
                return std::make_pair(0, Name());
            }
          IndexSkipList::const_iterator first =
            m_skipList.lower_bound(prev->getName().getPrefix(interest.getName().size() + 1));
          while (first != m_skipList.end() && first->getStatus() == DELETED)
            first++;
          IndexSkipList::const_iterator match =
                     std::find_if(first, last, bind(&matchesSimpleSelectors, interest, hash, _1));
          if (match != last)
            {
              return std::make_pair(match->getId(), match->getName());
            }
          last = first;
        }
    }
  return std::make_pair(0, Name());
}

Index::Entry::Entry(const Data& data, const int64_t id)
  : m_name(data.getFullName())
  , m_id(id)
  , m_status(EXISTED)
{
  const ndn::Signature& signature = data.getSignature();
  if (signature.hasKeyLocator())
    m_keyLocatorHash = computeKeyLocatorHash(signature.getKeyLocator());
}

Index::Entry::Entry(const Name& fullName, const KeyLocator& keyLocator, const int64_t id)
  : m_name(fullName)
  , m_keyLocatorHash(computeKeyLocatorHash(keyLocator))
  , m_id(id)
  , m_status(EXISTED)
{
}

Index::Entry::Entry(const Name& fullName,
                    const ndn::ConstBufferPtr& keyLocatorHash, const int64_t id)
  : m_name(fullName)
  , m_keyLocatorHash(keyLocatorHash)
  , m_id(id)
  , m_status(EXISTED)
{
}

Index::Entry::Entry(const Name& name)
  : m_name(name)
  , m_status(EXISTED)
{
}

} // namespace repo
