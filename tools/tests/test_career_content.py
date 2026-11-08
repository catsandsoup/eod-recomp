"""Career dealer content invariants (tools/career_content_check.py, docs/CAREER_UNLOCKS.md).

Synthetic cases always run. Set EOD_CAREER_CONTENT to a COMMON.RES (or a folder holding DATA/RES/COMMON.RES,
e.g. the integrated profile) to check the real content too; without it that case is skipped.
"""
import os
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import career_content_check as cc
import build_career_specials as bcs


def sec(name, **kw):
    return dict(name=name, parent='', pairs=list(kw.items()))


def buycar(*sections):
    return dict(sections=list(sections))


class DealerInvariants(unittest.TestCase):
    def test_clean_content_has_no_problems(self):
        b = buycar(sec('settings', car_price_multiplier='1.0'),
                   sec('car', chassis='Top', price='60'), sec('car', chassis='Phantom', price='1200'),
                   sec('vehicle', chassis='Top', number='865'), sec('vehicle', chassis='Phantom', number='776'))
        self.assertEqual(cc.check_dealer_content(b), [])

    def test_vehicle_without_car_is_reported(self):
        b = buycar(sec('car', chassis='Top', price='60'), sec('vehicle', chassis='Top', number='865'),
                   sec('vehicle', chassis='Daytona', number='900'))
        problems = cc.check_dealer_content(b)
        self.assertEqual(len(problems), 1)
        self.assertIn('Daytona', problems[0])

    def test_car_without_vehicle_and_duplicates_are_reported(self):
        b = buycar(sec('car', chassis='Top', price='60'), sec('car', chassis='Top', price='70'),
                   sec('car', chassis='Moth', price='85'), sec('vehicle', chassis='Top', number='865'),
                   sec('vehicle', chassis='Top', number='865'))
        text = ' | '.join(cc.check_dealer_content(b))
        self.assertIn('two [car]', text)
        self.assertIn('Moth has a [car] but no [vehicle]', text)
        self.assertIn('865 is listed twice', text)

    def test_chassis_needs_a_class_with_parts(self):
        b = buycar(sec('car', chassis='Top', price='60'), sec('car', chassis='Cupcar', price='1200'),
                   sec('vehicle', chassis='Top', number='865'), sec('vehicle', chassis='Cupcar', number='778'))
        classes = dict(sections=[sec('Compact'), sec('Midsize'), sec('$0000$chassis', Top='')])
        catalog = dict(sections=[sec('engine', car_class='compact')])
        text = ' | '.join(cc.check_dealer_content(b, classes, catalog))
        self.assertIn('Cupcar is in no car class', text)
        self.assertNotIn('Top', text)
        classes['sections'].append(sec('$0001$chassis', Cupcar=''))
        self.assertIn('Cupcar: class midsize has no part', ' | '.join(cc.check_dealer_content(b, classes, catalog)))
        catalog['sections'].append(sec('engine', car_class='midsize'))
        self.assertEqual(cc.check_dealer_content(b, classes, catalog), [])

    def test_special_vehicles_keep_every_invariant(self):
        b = buycar(sec('car', chassis='Top', price='60'), sec('vehicle', chassis='Top', number='865', body_color='1, 2, 3'))
        classes = dict(sections=[sec('Compact'), sec('Midsize'), sec('Muscle'), sec('Special'),
                                 sec('$0000$chassis', Top=''), sec('$0001$chassis'), sec('$0002$chassis'),
                                 sec('$0003$chassis', Postal='', Taxi='')], unk1=4)
        stats = dict(sections=[sec('carstats', chassis='Top', durability='2', handling='1', speed='1'),
                               sec('carstats', chassis='Postal', durability='2', handling='8', speed='6'),
                               sec('carstats', chassis='Taxi', durability='7', handling='6', speed='6')])
        catalog = dict(sections=[sec('engine', car_class=c) for c in ('compact', 'midsize', 'muscle')])
        specials = [s for s in bcs.SPECIALS if s['base'] in ('Postal', 'Taxi')]
        nb, nc, ns = bcs.add_specials(b, classes, stats, specials)
        self.assertEqual(cc.check_dealer_content(nb, nc, catalog), [])
        names = {dict(x['pairs']).get('chassis') for x in nb['sections'] if x['name'] == 'car'}
        self.assertEqual(names, {'Top', 'Mailvan', 'Cabbie'})
        by_name = {dict(x['pairs'])['chassis']: x for x in ns['sections']}
        # same ratings as the original chassis; the original keeps its Special class
        self.assertEqual(dict(by_name['Mailvan']['pairs'])['speed'], '6')
        self.assertEqual(dict(by_name['Mailvan']['pairs'])['handling'], '8')
        self.assertIn(('Postal', ''), nc['sections'][7]['pairs'])
        self.assertIn(('Mailvan', ''), nc['sections'][4]['pairs'])   # Compact
        self.assertIn(('Cabbie', ''), nc['sections'][5]['pairs'])    # Midsize
        self.assertEqual(nc['root_count'], 4)
        self.assertEqual(len(classes['sections'][4]['pairs']), 1)    # the input is untouched
        with self.assertRaises(ValueError):
            bcs.add_specials(nb, nc, ns, specials)                   # no second time

    def test_special_table_is_inside_the_peers_price_bands(self):
        import re
        for sp in bcs.SPECIALS:
            prices = [int(x) for x in re.findall(r'\d+', sp['peers'])]
            self.assertTrue(min(prices) <= sp['price'] <= max(prices), sp['name'])
            self.assertIn(sp['cls'], bcs.CLASS_SECTION)
        self.assertEqual(len({s['number'] for s in bcs.SPECIALS}), len(bcs.SPECIALS))
        self.assertEqual(len({s['name'].casefold() for s in bcs.SPECIALS}), len(bcs.SPECIALS))

    @unittest.skipUnless(os.environ.get('EOD_CAREER_CONTENT'), 'EOD_CAREER_CONTENT not set')
    def test_real_content(self):
        vals = cc._load_common_res(os.environ['EOD_CAREER_CONTENT'])
        self.assertEqual(cc.check_dealer_content(vals['buycar.val'], vals['carclass.val'], vals['catalog.val']), [])


if __name__ == '__main__':
    unittest.main()
