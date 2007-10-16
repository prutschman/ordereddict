

import timeit
import sys

import ordereddict
import odict
import ta   

LOOP=10000
MINOF=10
#LOOP=10
#MINOF=1

print 'loop:', LOOP, 'minof', MINOF
header = '--------------------------------- dict ordereddict OrderedDict'

def do_time():
    results = ordereddict.ordereddict()
    print sys.argv
    if len(sys.argv) > 1:
        todo = sys.argv[1:]
    else:
        todo = sorted([x for x in dir(ta.timeall) if x.startswith('time')])
    print header
    for funname in todo:
        fun = "%-30s" % (funname.split('_', 1)[1],)
        results[fun] = []
        print fun,
        for testdict in ("dict", "ordereddict.ordereddict", "odict.OrderedDict"):
            if testdict == "dict" and "nodict" in funname:
                res = None
                print '--------',
            else:
                t = timeit.Timer("ta.timeall(%s).%s()" % (testdict, funname), 
                             "import ta, ordereddict, odict")
                res = min(t.repeat(MINOF, LOOP))
                print '%8.3f' % (res,),
                sys.stdout.flush()
            results[fun].append(res)
        print
    print header
    for f, (x, y, z) in results.iteritems():
        print f,
        if x is None:
            print '--------', 
        else:
            print '%8.3f' % (x / y),
        print '   1.000',
        print '%8.3f' % (z / y)
        
        
        
    
if __name__ == "__main__":
    do_time()
    