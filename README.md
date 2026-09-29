```
PCI resource read/write utility

READ:
  ./pci [-b 8|16|32|64] [-l 4|8|16|32] <BDF> <resource> <offset> <count>

WRITE:
  ./pci [-b 8|16|32|64] -w <value> <BDF> <resource> <offset>

Options:
  -b <bits>     Access width (8,16,32,64), default=32
  -l <words>    Words per line (4,8,16,32), default=4
  -w <value>    Write value
  -h            Help

Examples:
  ./pci 01:00.0 0 0x0000 64
  ./pci -l 8 01:00.0 0 0x0000 64
  ./pci -b 16 -l 16 01:00.0 2 0x100 64
  ./pci -b 32 -l 32 01:00.0 0 0x1000 256
  ./pci -b 32 -w 0x12345678 01:00.0 0 0x100
```
