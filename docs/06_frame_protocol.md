# Frame wire protocol

Transport is one WebSocket binary message from `/ws`; HTTP is used for `/` and `/status`. Each message is `[28-byte header][payload]`.

| Offset | Field | Type |
|---:|---|---|
| 0 | magic | uint32 `0x4652414d` (`FRAM`) |
| 4 | width | uint32 |
| 8 | height | uint32 |
| 12 | pixel_format | uint32 V4L2 code |
| 16 | stride | uint32 bytes/row |
| 20 | frame_size | uint32 |
| 24 | sequence | uint32 |

Fields are seven 32-bit words, serialized as the host C struct (currently little-endian Linux). Payload is YUYV 4:2:2: bytes `Y0 U0 Y1 V0`, repeated; size is normally `stride*height` (640×480×2 = 614400 bytes). Approximate rate is `width*height*2*fps`; at 640×480×30 it is 36.864 MB/s before WS/TCP overhead.

| Item | Current behavior |
|---|---|
| integer byte order | native/little-endian in deployed targets |
| sequence | low 32 bits of frame sequence |
| framing | WebSocket message boundary |

Example (header for 640×480, format `0x56595559`, stride 1280, size 614400, sequence 1):
```text
4d 52 41 46 80 02 00 00 e0 01 00 00 59 55 59 56 00 05 00 00 00 60 09 00 01 00 00 00
```

Browser sketch:
```js
ws.binaryType = 'arraybuffer';
ws.onmessage = e => { const b=new Uint8Array(e.data), d=new DataView(e.data);
  if (d.getUint32(0,true)!==0x4652414d) return;
  const w=d.getUint32(4,true), h=d.getUint32(8,true), stride=d.getUint32(16,true);
  const size=d.getUint32(20,true); const yuyv=b.subarray(28,28+size); /* convert rows */
};
```
Limitations are raw bandwidth, native-endian header, and no compression/authentication/negotiation. Future extensions: explicit endian marker, 64-bit sequence, timestamps, format negotiation, compression, and authentication. Multiple receivers are already supported (broadcast).

## Parsing safely
Never trust a received length. First require at least 28 bytes, read the seven fields, verify magic, require `frame_size <= message.length - 28`, and reject dimensions/stride that would overflow an allocation or canvas calculation. Treat a partial or malformed message as a dropped frame, not as a reason to index past the buffer. The current C sender uses `memcpy` of its native struct; a portable implementation should explicitly encode each word little-endian rather than relying on struct layout.

## Worked payload
For two pixels `(Y0,U,Y1,V)`, both pixels share U and V: pixel zero uses Y0 and pixel one uses Y1. Row padding, if `stride > width*2`, must be skipped at the end of every row. `sequence` is useful for detecting drops; it is not a timestamp and wraps at 2^32. WebSocket itself supplies message framing, masking rules, and TCP ordering; this protocol supplies only video semantics.
