/** \file
    \brief Tonk Implementation: Pre-compiled header
    \copyright Copyright (c) 2017 Christopher A. Taylor.  All rights reserved.
*/

#pragma once

// When doing large refactors it can be helpful to turn off PCH so that
// single modules can be test-compiled in isolation.
#define TONK_ENABLE_PCH

#ifdef TONK_ENABLE_PCH
#include "siamese.h"
#include "SiameseCommon.h"
#include "tonk.h"
#include "TonkineseTools.h"
#include "TonkineseBandwidth.h"
#include "TonkineseIncoming.h"
#include "TonkineseOutgoing.h"
#include "TonkineseConnection.h"
#include "TonkineseProtocol.h"
#include "TonkineseSession.h"
#include "TonkineseTools.h"
#include "TonkineseUDP.h"
#include "TonkineseNAT.h"
#include "TonkineseFlood.h"
#include "PacketCompression.h"
#include "TimeSync.h"
#include "StrikeRegister.h"
#include "SimpleCipher.h"
#endif // TONK_ENABLE_PCH
