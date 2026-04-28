def f(a, b):
  return a + b

def g(a, b):
  a = a + 2 * b
  c = f(a, b)
  return a * b + c

if __name__ == "__main__":
    a = 1
    b = 2
    g(a, b)
    print(a, b)
