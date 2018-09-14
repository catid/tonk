/** \file
    \brief Tonk Implementation: Connection Maps
    \copyright Copyright (c) 2017-2018 Christopher A. Taylor.  All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice,
      this list of conditions and the following disclaimer in the documentation
      and/or other materials provided with the distribution.
    * Neither the name of Tonkinese nor the names of its contributors may be
      used to endorse or promote products derived from this software without
      specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
    AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
    IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
    ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
    LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
    CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
    SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
    INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
    CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
    ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
    POSSIBILITY OF SUCH DAMAGE.
*/

#pragma once

#include "TonkineseTools.h"
#include "TonkineseInterfaces.h"

namespace tonk {


//------------------------------------------------------------------------------
// LightHashMap

/**
    LightHashMap

    A super-fast unordered map.
    It does not work with std::string, only with Plain-Old-Datatypes (POD).

    This is based on Richard Geldreich's hash_map class in Crunch (crnlib).
    It has been altered to remove the STL-like features, to integrate with
    the Tonk codebase, and to remove support for non-POD keys/values.

    Motivation: Since short key/value maps are used in a few critical paths
    in Tonk, it seems worthwhile to accelerate these.  Having access to the
    implementation of the hash map allows me to avoid a double-lookup for
    insert-if-new operations that are common.

    The LoC count is small since I can remove unused code.
    Since it is copied from well-tested code, it's trust-worthy.
*/

//------------------------------------------------------------------------------
//
// crnlib uses the ZLIB license:
// http://opensource.org/licenses/Zlib
//
// Copyright (c) 2010-2016 Richard Geldreich, Jr. and Binomial LLC
//
// This software is provided 'as-is', without any express or implied
// warranty.  In no event will the authors be held liable for any damages
// arising from the use of this software.
//
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
//
// 1. The origin of this software must not be misrepresented; you must not
// claim that you wrote the original software. If you use this software
// in a product, an acknowledgment in the product documentation would be
// appreciated but is not required.
//
// 2. Altered source versions must be plainly marked as such, and must not be
// misrepresented as being the original software.
//
// 3. This notice may not be removed or altered from any source distribution.
//
//------------------------------------------------------------------------------

template<typename Key, typename Value, typename Hasher>
struct LightHashMap
{
    enum state
    {
        cStateInvalid = 0,
        cStateValid = 1
    };

    static const int cMinHashSize = 4U;

    using hash_map_type = LightHashMap<Key, Value, Hasher>;
    using value_type = std::pair<Key, Value>;

    unsigned    m_hash_shift = 32;
    Hasher      m_hasher;
    unsigned    m_num_valid = 0;
    unsigned    m_grow_threshold = 0;

    // insert_result.first will always point to inserted key/value (or the already existing key/value).
    // insert_resutt.second will be true if a new key/value was inserted, or false if the key already existed (in which case first will point to the already existing value).
    // The first value in the pair is the find_key() result
    typedef std::pair<int, bool> insert_result;

    inline LightHashMap() {}
    inline ~LightHashMap()
    {
        delete[] m_values;
    }

    LightHashMap(const LightHashMap& other) = delete;
    LightHashMap& operator=(const LightHashMap& other) = delete;

    inline unsigned size() const
    {
        return m_num_valid;
    }

    inline unsigned get_table_size()
    {
        return m_values_count;
    }

    inline bool empty()
    {
        return 0 == m_num_valid;
    }

    inline insert_result insert(const Key& k, const Value& v = Value())
    {
        insert_result result;
        if (!insert_no_grow(result, k, v))
        {
            unsigned newSize = m_values_count * 2;
            if (newSize < cMinHashSize) {
                newSize = cMinHashSize;
            }

            rehash(newSize);

            if (!insert_no_grow(result, k, v)) {
                TONK_DEBUG_BREAK();
            }
        }

        return result;
    }

    inline void erase_index(int i)
    {
        TONK_DEBUG_ASSERT(i >= 0 && i < (int)m_values_count);

        node* pDst = get_node(i);
        pDst->state = cStateInvalid;

        m_num_valid--;

        for (;;)
        {
            int r, j = i;

            node* pSrc = pDst;

            do
            {
                if (!i)
                {
                    i = m_values_count - 1;
                    pSrc = get_node(i);
                }
                else
                {
                    i--;
                    pSrc--;
                }

                if (!pSrc->state) {
                    return;
                }

                r = hash_key(pSrc->first);

            } while ((i <= r && r < j) ||
                (r < j && j < i) ||
                (j < i && i <= r));

            move_node(pDst, pSrc);

            pDst = pSrc;
        }
    }

    struct node : public value_type
    {
        uint8_t state;
    };

    static inline void move_node(node* pDst, node* pSrc)
    {
        TONK_DEBUG_ASSERT(!pDst->state);
        *pDst = *pSrc;
        pSrc->state = cStateInvalid;
    }

    struct raw_node
    {
        inline raw_node()
        {
            node* p = reinterpret_cast<node*>(this);
            p->state = cStateInvalid;
        }

        inline ~raw_node()
        {
        }

        inline raw_node(const raw_node& other)
        {
            node* pDst = reinterpret_cast<node*>(this);
            const node* pSrc = reinterpret_cast<const node*>(&other);

            if (pSrc->state)
            {
                *pDst = *pSrc;
                pDst->state = cStateValid;
            }
            else {
                pDst->state = cStateInvalid;
            }
        }

        inline raw_node& operator= (const raw_node& rhs)
        {
            if (this == &rhs) {
                return *this;
            }

            node* pDst = reinterpret_cast<node*>(this);
            const node* pSrc = reinterpret_cast<const node*>(&rhs);

            if (pSrc->state)
            {
                if (pDst->state)
                {
                    pDst->first = pSrc->first;
                    pDst->second = pSrc->second;
                }
                else
                {
                    *pDst = *pSrc;
                    pDst->state = cStateValid;
                }
            }
            else if (pDst->state)
            {
                pDst->state = cStateInvalid;
            }

            return *this;
        }

        uint8_t m_bits[sizeof(node)];
    };

    raw_node* m_values = nullptr;
    unsigned m_values_count = 0;

    inline unsigned hash_key(const Key& k) const
    {
        TONK_DEBUG_ASSERT((1U << (32U - m_hash_shift)) == m_values_count);

        unsigned hash = static_cast<unsigned>(m_hasher(k));

        // Fibonacci hashing
        hash = (2654435769U * hash) >> m_hash_shift;

        TONK_DEBUG_ASSERT(hash < m_values_count);
        return hash;
    }

    inline const node* get_node(unsigned index) const
    {
        const raw_node* raw = m_values + index;
        return reinterpret_cast<const node*>(raw);
    }

    inline node* get_node(unsigned index)
    {
        raw_node* raw = m_values + index;
        return reinterpret_cast<node*>(raw);
    }

    inline state get_node_state(unsigned index) const
    {
        return static_cast<state>(get_node(index)->state);
    }

    inline void rehash(unsigned new_hash_size)
    {
        TONK_DEBUG_ASSERT(new_hash_size >= m_num_valid);
        TONK_DEBUG_ASSERT((new_hash_size & (new_hash_size - 1)) == 0); // ispow2

        if ((new_hash_size < m_num_valid) ||
            (new_hash_size == m_values_count))
        {
            return;
        }

        raw_node* old_values = m_values;
        const unsigned old_values_count = m_values_count;
        const unsigned old_num_valid = m_num_valid;

        m_values = new raw_node[new_hash_size];
        m_values_count = new_hash_size;

        m_num_valid = 0;
        const unsigned bitIndex = NonzeroLowestBitIndex(new_hash_size);
        m_hash_shift = 32 - bitIndex;
        TONK_DEBUG_ASSERT(new_hash_size == (1U << (32 - m_hash_shift)));

        for (unsigned i = 0; i < old_values_count; ++i)
        {
            node* pNode = reinterpret_cast<node*>( old_values + i );

            if (pNode->state)
            {
                move_into(pNode);

                if (old_num_valid == m_num_valid) {
                    break;
                }
            }
        }

        m_grow_threshold = (new_hash_size + 1) >> 1;

        delete[] old_values;
    }

    inline int find_index(const Key& k) const
    {
        if (m_num_valid)
        {
            unsigned index = hash_key(k);
            const node* pNode = get_node(index);

            if (pNode->state)
            {
                if (pNode->first == k) {
                    return index;
                }

                const unsigned orig_index = index;

                for (;;)
                {
                    if (!index)
                    {
                        index = m_values_count - 1;
                        pNode = get_node(index);
                    }
                    else
                    {
                        index--;
                        pNode--;
                    }

                    if (index == orig_index) {
                        break;
                    }

                    if (!pNode->state) {
                        break;
                    }

                    if (pNode->first == k) {
                        return index;
                    }
                }
            }
        }

        return -1; // Not found
    }

    inline bool insert_no_grow(insert_result& result, const Key& k, const Value& v = Value())
    {
        if (!m_values_count) {
            return false;
        }

        int index = hash_key(k);
        node* pNode = get_node(index);

        if (pNode->state)
        {
            if (pNode->first == k)
            {
                result.first = index;
                result.second = false;
                return true;
            }

            const int orig_index = index;

            for (;;)
            {
                if (!index)
                {
                    index = m_values_count - 1;
                    pNode = get_node(index);
                }
                else
                {
                    index--;
                    pNode--;
                }

                if (orig_index == index) {
                    return false;
                }

                if (!pNode->state) {
                    break;
                }

                if (pNode->first == k)
                {
                    result.first = index;
                    result.second = false;
                    return true;
                }
            }
        }

        if (m_num_valid >= m_grow_threshold) {
            return false;
        }

        pNode->first = k;
        pNode->second = v;
        pNode->state = cStateValid;

        m_num_valid++;
        TONK_DEBUG_ASSERT(m_num_valid <= m_values_count);

        result.first = index;
        result.second = true;

        return true;
    }

    inline void move_into(node* pNode)
    {
        int index = hash_key(pNode->first);
        node* pDst_node = get_node(index);

        if (pDst_node->state)
        {
            const int orig_index = index;

            for (;;)
            {
                if (!index)
                {
                    index = m_values_count - 1;
                    pDst_node = get_node(index);
                }
                else
                {
                    index--;
                    pDst_node--;
                }

                if (index == orig_index)
                {
                    TONK_DEBUG_ASSERT(false);
                    return;
                }

                if (!pDst_node->state) {
                    break;
                }
            }
        }

        move_node(pDst_node, pNode);

        m_num_valid++;
    }
};


//------------------------------------------------------------------------------
// ConnectionMap

/// Templated map from a key to a Connection*
template<typename KeyT, typename HashT> class ConnectionMap
{
    /// Map from UDP datagram source address to Connection object
    using MapT = LightHashMap<KeyT, IConnection*, HashT>;

    MapT TheMap;

public:
    /// Attempt to insert a key.
    /// Returns true if the key was unique
    /// Returns false if the key was already found
    TONK_FORCE_INLINE bool Insert(const KeyT& key, IConnection* conn)
    {
        auto result = TheMap.insert(key, conn);
        return result.second;
    }

    /// Attempt to remove a key.
    /// Returns true if the key was found
    /// Returns false if the key was NOT found
    bool Remove(const KeyT& key, IConnection* conn)
    {
        int index = TheMap.find_index(key);
        if (index < 0) {
            return false;
        }
        if (TheMap.get_node(index)->second != conn) {
            return false;
        }
        TheMap.erase_index(index);
        return true;
    }

    /// Attempt to find a key.
    /// Returns nullptr if key was not found
    TONK_FORCE_INLINE IConnection* Find(const KeyT& key) const
    {
        int index = TheMap.find_index(key);
        if (index < 0) {
            return nullptr;
        }
        return TheMap.get_node(index)->second;
    }

    /// Return number of elements in the map
    TONK_FORCE_INLINE unsigned GetCount() const
    {
        return TheMap.size();
    }
};


//------------------------------------------------------------------------------
// ConnectionIdMap

/**
    This is a map from a unique number assigned to each connection, the
    Connection Id, and Connection.
*/
class ConnectionIdMap
{
public:
    /// Id datatype
    typedef uint32_t id_t;

    TONK_FORCE_INLINE virtual ~ConnectionIdMap()
    {
        TONK_DEBUG_ASSERT(IdMap.GetCount() == 0);
    }

    ConnectionIdMap();


    TONK_FORCE_INLINE unsigned GetConnectionCount() const
    {
        Locker locker(IdLock);
        return IdMap.GetCount();
    }

    /// Try to find a connection by identifier.
    /// Increments the connection reference count.
    /// Returns nullptr if id was not found
    TONK_FORCE_INLINE IConnection* FindById(id_t id)
    {
        TONK_DEBUG_ASSERT(id != TONK_INVALID_ID);
        Locker locker(IdLock);
        IConnection* connection = IdMap.Find(id);
        if (connection) {
            connection->SelfRefCount.IncrementReferences();
        }
        return connection;
    }

    /// Assign an id and insert connection into id map
    /// Return false if address or id was already found
    bool InsertAndAssignId(
        id_t& idOut,
        IConnection* connection);

    /// Remove connection from maps
    /// Returns false if not found
    bool RemoveAndUnassignId(
        id_t id,
        IConnection* connection);

protected:
    mutable Lock IdLock;
    ConnectionMap<id_t, UInt32Hasher> IdMap;

    /// Optimization: Attempt to reuse 1-byte ids as often as possible
    uint8_t BestUnusedIds[256];
    unsigned BestUnusedCount = 256;

    /// Next Id to allocate
    id_t NextId = 0;
};


//------------------------------------------------------------------------------
// ConnectionAddrMap

/// This is a map from a source address to a Connection.
class ConnectionAddrMap
{
public:
    TONK_FORCE_INLINE virtual ~ConnectionAddrMap()
    {
        TONK_DEBUG_ASSERT(AddressMap.GetCount() == 0);
    }


    /// Find a connection by address.
    /// Increments the connection reference count.
    /// Returns nullptr if source address was not found
    IConnection* FindByAddr(const UDPAddress& addr);

    /// Insert connection into source address map
    /// Return false if address was already found
    bool InsertAddress(
        const UDPAddress& addr,
        IConnection* connection);

    /// Remove connection from maps
    /// Returns false if not found
    bool RemoveAddress(
        const UDPAddress& addr,
        IConnection* connection);

    /// Update the UDP address of the peer
    /// Returns false if the new address is already in the map
    bool ChangeAddress(
        const UDPAddress& oldAddress,
        const UDPAddress& newAddress,
        IConnection* connection);

protected:
    mutable Lock AddressMapLock;
    ConnectionMap<UDPAddress, UDPAddressHasher> AddressMap;

    /// Optimization: Often times datagrams are received from the same address
    UDPAddress RecentAddress;
    IConnection* RecentConnection;
};


//------------------------------------------------------------------------------
// P2PConnectionKeyMap

/// This is a map from a 32 - bit P2P assigned EncryptionId and a Connection.
class P2PConnectionKeyMap
{
public:
    /// Key datatype
    typedef uint32_t key_t;

    TONK_FORCE_INLINE virtual ~P2PConnectionKeyMap()
    {
        TONK_DEBUG_ASSERT(P2PKeyMap.GetCount() == 0);
    }


    /// Try to handle a datagram by identifier.
    /// Increments the connection reference count.
    /// Returns nullptr if key was not found
    IConnection* FindByKey(key_t key)
    {
        Locker locker(P2PKeyLock);
        IConnection* connection = P2PKeyMap.Find(key);
        if (connection) {
            connection->SelfRefCount.IncrementReferences();
        }
        return connection;
    }

    /// Insert connection into key map
    /// Return false if key was already found
    TONK_FORCE_INLINE bool InsertByKey(
        key_t key,
        IConnection* connection)
    {
        Locker locker(P2PKeyLock);
        return P2PKeyMap.Insert(key, connection);
    }

    /// Remove connection from maps
    /// Returns false if not found
    TONK_FORCE_INLINE bool RemoveByKey(
        key_t key,
        IConnection* connection)
    {
        Locker locker(P2PKeyLock);
        return P2PKeyMap.Remove(key, connection);
    }

protected:
    mutable Lock P2PKeyLock;
    ConnectionMap<key_t, UInt32Hasher> P2PKeyMap;
};


} // namespace tonk
