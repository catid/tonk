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

#include "TonkineseMaps.h"

namespace tonk {

static logger::Channel ModuleLogger("Maps", MinimumLogLevel);


//------------------------------------------------------------------------------
// ConnectionIdMap

ConnectionIdMap::ConnectionIdMap()
{
    // Add a different offset for each socket so that bugs where "my" and "peer"
    // ids are swapped can be found more easily.
    const uint8_t randomOffset = static_cast<uint8_t>(siamese::GetTimeUsec() % 256);

    for (unsigned i = 0; i < 256; ++i) {
        BestUnusedIds[i] = (uint8_t)(255 - i + randomOffset);
    }
}

bool ConnectionIdMap::InsertAndAssignId(
    id_t& idOut,
    IConnection* connection)
{
    Locker locker(IdLock);

    if (BestUnusedCount > 0)
    {
        idOut = BestUnusedIds[--BestUnusedCount];
        const bool success = IdMap.Insert(idOut, connection);
        if (!success) {
            ++BestUnusedCount;
        }
        //ModuleLogger.Trace("ID ADD ", idOut, " + ", connection, " - ", success);
        return success;
    }

    if (IdMap.GetCount() >= protocol::kConnectionIdCount)
    {
        TONK_DEBUG_BREAK(); // Should never happen
        return false;
    }

    // Find next unused Id
    do
    {
        ++NextId;
        if (NextId < 256 || NextId >= protocol::kConnectionIdCount) {
            NextId = 256;
        }
    } while (!IdMap.Insert(NextId, connection));

    idOut = NextId;
    return true;
}

bool ConnectionIdMap::RemoveAndUnassignId(
    id_t id,
    IConnection* connection)
{
    if (id == TONK_INVALID_ID) {
        return false;
    }

    Locker locker(IdLock);

    // If the ID is just one byte:
    if (id < 256 && BestUnusedCount < 256) {
        // Put it back in the list of one byte IDs
        BestUnusedIds[BestUnusedCount++] = (uint8_t)id;
    }

    bool success = IdMap.Remove(id, connection);
    //ModuleLogger.Trace("ID REMOVE ", id, " + ", connection, " - ", success);
    return success;
}


//------------------------------------------------------------------------------
// ConnectionAddrMap

IConnection* ConnectionAddrMap::FindByAddr(const UDPAddress& addr)
{
    TONK_DEBUG_ASSERT(!addr.address().is_unspecified());

    Locker locker(AddressMapLock);

    IConnection* connection;

    if (addr == RecentAddress) {
        connection = RecentConnection;
    }
    else
    {
        connection = AddressMap.Find(addr);
        if (connection)
        {
            RecentAddress = addr;
            RecentConnection = connection;
        }
    }

    if (connection) {
        connection->SelfRefCount.IncrementReferences();
    }
    return connection;
}

bool ConnectionAddrMap::InsertAddress(
    const UDPAddress& addr,
    IConnection* connection)
{
    if (addr.address().is_unspecified())
    {
        TONK_DEBUG_BREAK(); // Should never happen
        return false;
    }

    Locker locker(AddressMapLock);
    return AddressMap.Insert(addr, connection);
}

bool ConnectionAddrMap::RemoveAddress(
    const UDPAddress& addr,
    IConnection* connection)
{
    if (addr.address().is_unspecified()) {
        return false;
    }

    Locker locker(AddressMapLock);

    // Remove from recent list
    if (RecentConnection == connection)
    {
        RecentAddress = UDPAddress();
        RecentConnection = nullptr;
    }

    return AddressMap.Remove(addr, connection);
}

bool ConnectionAddrMap::ChangeAddress(
    const UDPAddress& oldAddress,
    const UDPAddress& newAddress,
    IConnection* connection)
{
    Locker locker(AddressMapLock);

    // Remove from recent list
    if (RecentConnection == connection)
    {
        RecentAddress = UDPAddress();
        RecentConnection = nullptr;
    }

    if (!oldAddress.address().is_unspecified()) {
        AddressMap.Remove(oldAddress, connection);
    }
    if (!newAddress.address().is_unspecified()) {
        return AddressMap.Insert(newAddress, connection);
    }

    // If new address is unspecified, return true
    return true;
}


} // namespace tonk
