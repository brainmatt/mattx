import socket
import struct

# Header: type (u32), length (u32), sender_id (u32)
# MATTX_MSG_HEARTBEAT = 1
header = struct.pack("III", 1, 0, 99) 

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.connect(("127.0.0.1", 7226))
s.send(header)
s.close()
print("Heartbeat sent!")



