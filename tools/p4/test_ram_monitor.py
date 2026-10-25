#!/usr/bin/env python3
import json
import unittest
from ram_monitor import parse_sample, parse_event


class Samples(unittest.TestCase):
    def setUp(self):
        self.sample = {'schema': 1, 'dram': {'total': 457235, 'free': 175727, 'minFree': 31812},
                       'psram': {'total': 32959876, 'free': 32535496},
                       'heapCaps': {'internalLargest': 98304}}

    def line(self):
        return '05:16:28 $ ' + json.dumps(self.sample, separators=(',', ':')) + '\r'

    def test_real_schema_and_only_memory_fields(self):
        self.sample['secret'] = 'never-export-this'
        result = parse_sample(self.line(), 123)
        self.assertEqual(result['internalFree'], 175727)
        self.assertEqual(result['internalLargest'], 98304)
        self.assertEqual(result['psramFree'], 32535496)
        self.assertEqual(result['t'], 123)
        self.assertNotIn('secret', result)

    def test_partial_or_unrelated_line_is_not_a_sample(self):
        self.assertIsNone(parse_sample(self.line()[:100], 0))
        self.assertIsNone(parse_sample('login private-value', 0))
        self.assertIsNone(parse_sample('{"schema":1,"other":22}', 0))

    def test_invalid_numbers_and_impossible_totals(self):
        for value in (-1, True, '175727', float('nan'), 999999999):
            self.sample['dram']['free'] = value
            self.assertIsNone(parse_sample(self.line(), 0))

    def test_no_psram(self):
        self.sample['psram'] = {'available': False}
        result = parse_sample(self.line(), 0)
        self.assertEqual(result['psramFree'], 0)
        self.assertEqual(result['psramTotal'], 0)

    def test_events_do_not_expose_raw_logs(self):
        self.assertEqual(parse_event('STT: TL warm ok=0 private-payload', 1),
                         {'t': 1, 'label': 'Speech warm-up failed'})
        self.assertEqual(parse_event('STT: TL seg=1 ok=1 private-payload', 2),
                         {'t': 2, 'label': 'Speech segment succeeded'})
        self.assertIsNone(parse_event('a private transcription', 3))


if __name__ == '__main__':
    unittest.main()
