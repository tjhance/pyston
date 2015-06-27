def f():
    d = dict(a=1, b=u"x", c=False, d="y", e=True, f=5)
    print sorted(list(d.iteritems()))

for i in xrange(100):
    f()

# (something similar was failing in a sqlalchemy_imperative test)
