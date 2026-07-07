import sys
# Turn a binary file into a C byte-array header.
#   python gen_header.py <infile> <NAME> <outfile>
infile, name, outfile = sys.argv[1], sys.argv[2], sys.argv[3]
data = open(infile, "rb").read()
with open(outfile, "w") as f:
    f.write("/* Auto-generated from %s. Do not edit. */\n" % name.lower())
    f.write("static const unsigned char %s_BYTES[] = {\n" % name)
    for i in range(0, len(data), 16):
        f.write("  " + ",".join("0x%02x" % b for b in data[i:i+16]) + ",\n")
    f.write("};\n")
    f.write("static const unsigned int %s_LEN = %du;\n" % (name, len(data)))
print("%s -> %s  (%d bytes)" % (infile, outfile, len(data)))
