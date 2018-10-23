import sys
fp = open(sys.argv[1])
old_val = -1
val = -1
err_count = 0
for line in fp:
    val = int(line)
    if val != (old_val+1):
        err_count = err_count + 1
        print("val="+str(val) + " old_val=" + str(old_val))
        val = old_val + 1
    old_val = val
    
if val == 10000 -1 and err_count == 0:
    print("success " + sys.argv[1] + "\n")
else:
    print("fail " + sys.argv[1] + "\n")
