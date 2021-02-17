/*
 *
 *    Copyright (c) 2021 Project CHIP Authors
 *    All rights reserved.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

/**
 *    @file
 *      This file defines functions for encoding and decoding CHIP messages.
 *      The encoded messages contain CHIP packet header, encrypted payload
 *      header, encrypted payload and message authentication code, as per
 *      CHIP specifications.
 *
 */

#include <support/CodeUtils.h>
#include <support/ReturnMacros.h>
#include <support/SafeInt.h>
#include <transport/SecureMessageCodec.h>

// Maximum length of application data that can be encrypted as one block.
// The limit is derived from IPv6 MTU (1280 bytes) - expected header overheads.
// This limit would need additional reviews once we have formalized Secure Transport header.
//
// TODO: this should be checked within the transport message sending instead of the session management layer.
static const size_t kMax_SecureSDU_Length = 1024;

namespace chip {

using System::PacketBuffer;
using System::PacketBufferHandle;

namespace SecureMessageCodec {

CHIP_ERROR Encode(NodeId localNodeId, Transport::PeerConnectionState * state, PayloadHeader & payloadHeader,
                  PacketHeader & packetHeader, System::PacketBufferHandle & msgBuf)
{
    VerifyOrReturnError(!msgBuf.IsNull(), CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(!msgBuf->HasChainedBuffer(), CHIP_ERROR_INVALID_MESSAGE_LENGTH);
    VerifyOrReturnError(msgBuf->TotalLength() < kMax_SecureSDU_Length, CHIP_ERROR_INVALID_MESSAGE_LENGTH);

    uint32_t msgId = state->GetSendMessageIndex();

    static_assert(std::is_same<decltype(msgBuf->TotalLength()), uint16_t>::value,
                  "Addition to generate payloadLength might overflow");

    packetHeader
        .SetSourceNodeId(localNodeId)                 //
        .SetDestinationNodeId(state->GetPeerNodeId()) //
        .SetMessageId(msgId)                          //
        .SetEncryptionKeyID(state->GetLocalKeyID());
    packetHeader.GetFlags().Set(Header::FlagValues::kSecure);

    ReturnErrorOnFailure(payloadHeader.EncodeBeforeData(msgBuf));

    uint8_t * data    = msgBuf->Start();
    uint16_t totalLen = msgBuf->TotalLength();

    MessageAuthenticationCode mac;
    ReturnErrorOnFailure(state->EncryptBeforeSend(data, totalLen, data, packetHeader, mac));

    uint16_t taglen = 0;
    ReturnErrorOnFailure(mac.Encode(packetHeader, &data[totalLen], msgBuf->AvailableDataLength(), &taglen));

    VerifyOrReturnError(CanCastTo<uint16_t>(totalLen + taglen), CHIP_ERROR_INTERNAL);
    msgBuf->SetDataLength(static_cast<uint16_t>(totalLen + taglen));

    ChipLogDetail(Inet, "Secure message was encrypted: Msg ID %u", msgId);

    state->IncrementSendMessageIndex();
    return CHIP_NO_ERROR;
}

CHIP_ERROR Decode(Transport::PeerConnectionState * state, PayloadHeader & payloadHeader, const PacketHeader & packetHeader,
                  System::PacketBufferHandle & msg)
{
    ReturnErrorCodeIf(msg.IsNull(), CHIP_ERROR_INVALID_ARGUMENT);

    uint8_t * data = msg->Start();
    uint16_t len   = msg->DataLength();

    PacketBufferHandle origMsg;
#if CHIP_SYSTEM_CONFIG_USE_LWIP
    /* This is a workaround for the case where PacketBuffer payload is not
        allocated as an inline buffer to PacketBuffer structure */
    origMsg = std::move(msg);
    msg     = PacketBufferHandle::New(len);
    VerifyOrReturnError(!msg.IsNull(), CHIP_ERROR_NO_MEMORY);
    msg->SetDataLength(len);
#endif

    uint16_t footerLen = MessageAuthenticationCode::TagLenForEncryptionType(packetHeader.GetEncryptionType());
    VerifyOrReturnError(footerLen <= len, CHIP_ERROR_INVALID_MESSAGE_LENGTH);

    uint16_t taglen = 0;
    MessageAuthenticationCode mac;
    ReturnErrorOnFailure(mac.Decode(packetHeader, &data[len - footerLen], footerLen, &taglen));
    VerifyOrReturnError(taglen == footerLen, CHIP_ERROR_INTERNAL);

    len = static_cast<uint16_t>(len - taglen);
    msg->SetDataLength(len);

    uint8_t * plainText = msg->Start();
    ReturnErrorOnFailure(state->DecryptOnReceive(data, len, plainText, packetHeader, mac));

    ReturnErrorOnFailure(payloadHeader.DecodeAndConsume(msg));
    return CHIP_NO_ERROR;
}

} // namespace SecureMessageCodec

} // namespace chip
