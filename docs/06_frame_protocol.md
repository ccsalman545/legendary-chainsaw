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
Limitations are one client, raw bandwidth, native-endian header, no compression/authentication/negotiation. Future extensions: explicit endian marker, 64-bit sequence, timestamps, format negotiation, compression, authentication, multi-client routing.
