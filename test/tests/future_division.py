"docstring"

from __future__ import division

def test(a, b):
    print a / b
    t = a
    t /= b
    print t

    print a // b
    t = a
    t //= b
    print t


test(3, 2)
test(3, 2.0)
test(3.0, 2)
test(3.0, 2.0)

print 3 / 2.0

print 3 // 2
b = 3
b //= 2
print b

class C(object):
    def __div__(self, other):
        print "__div__ shouldn't be called"

    def __truediv__(self, other):
        print "__truediv__ called"
        return 5

c = C()
d = D()
print c / d

c /= d
print c

class C(object):
    def __div__(self, other):
        print "__div__ shouldn't be called"

    def __itruediv__(self, other):
        print "__truediv__ called"
        return 5

c = C()
d = D()
c /= d
print c
