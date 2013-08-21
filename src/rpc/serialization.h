// Copyright (c) 2013, Cloudera, inc.
// All rights reserved.

#ifndef KUDU_RPC_SERIALIZATION_H
#define KUDU_RPC_SERIALIZATION_H

#include <inttypes.h>
#include <string.h>

namespace google {
namespace protobuf {
class MessageLite;
} // namespace protobuf
} // namespace google

namespace kudu {

class Status;
class faststring;
class Slice;

namespace rpc {
namespace serialization {

// Serialize the request param into a buffer which is allocated by this function.
// In: Protobuf Message to serialize
// Out: faststring to be populated with the serialized bytes.
Status SerializeMessage(const google::protobuf::MessageLite& message,
                        faststring* param_buf);

// Serialize the request or response header into a buffer which is allocated
// by this function.
// Includes leading 32-bit length of the buffer.
// In: Protobuf Header to serialize,
//     Length of the message param following this header in the frame.
// Out: faststring to be populated with the serialized bytes.
Status SerializeHeader(const google::protobuf::MessageLite& header,
                       size_t param_len,
                       faststring* header_buf);

// Deserialize the request.
// In: data buffer Slice.
// Out: parsed_header PB initialized,
//      parsed_main_message pointing to offset in original buffer containing
//      the main payload.
Status ParseMessage(const Slice& buf,
                    google::protobuf::MessageLite* parsed_header,
                    Slice* parsed_main_message);

// Serialize the RPC connection header (magic number + flags).
// buf must have 7 bytes available (kMagicNumberLength + kHeaderFlagsLength).
void SerializeConnHeader(uint8_t* buf);

// Validate the entire rpc header (magic number + flags).
Status ValidateConnHeader(const Slice& slice);


} // namespace serialization
} // namespace rpc
} // namespace kudu
#endif // KUDU_RPC_SERIALIZATION_H
