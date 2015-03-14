def f1(x):
    exec """
for i in xrange(x):
    pass
print x
    """

f1(10000)

def f3():
    exec """
def f2(x):
    def inner():
        return x
    return inner
    """
    g = f2(10000)
    for i in xrange(10000):
        g()
    print g()
f3()
