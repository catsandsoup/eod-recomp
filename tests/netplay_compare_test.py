import importlib.util, pathlib, unittest
p=pathlib.Path(__file__).resolve().parents[1]/'tools/netplay_compare.py'
class Compare(unittest.TestCase):
 def test_comparator_exists(self): self.assertTrue(p.exists(), 'strict replay comparator is missing')
 def test_comparison(self):
  if not p.exists(): self.skipTest('not implemented')
  s=importlib.util.spec_from_file_location('compare',p); m=importlib.util.module_from_spec(s); s.loader.exec_module(m)
  a=[{'event':'tick','step':1,'cars':[{'x':123}]}]
  self.assertIsNone(m.first_difference(a,a))
  self.assertEqual(m.first_difference(a,[{'event':'tick','step':1,'cars':[{'x':124}]}]), '$[0].cars[0].x: 123 != 124')
  self.assertIn('length',m.first_difference(a,[]))
  self.assertIsNotNone(m.first_difference([{'unknown':1}],[{'unknown':2}]))
if __name__ == '__main__':
 unittest.main()
