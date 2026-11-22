"""Check the frozen scenario oracle without relying on a generated implementation."""
import copy
import unittest
from .visual import SCENARIO
from .visual_checks import expected


class VisualOracleTests(unittest.TestCase):
    def test_independent_transition_simulation_matches_golden_report(self):
        jobs, events, claims, last = {}, [], [], {}
        def event(kind, job):
            events.append(dict(seq=len(events)+1, kind=kind, job_id=job['id']))
        def clear(job):
            job.update(lease_token=None, lease_until=None, worker=None)
        for index, source in enumerate(SCENARIO['jobs'], 1):
            job = copy.deepcopy(source)
            job.update(state='queued', attempts=0, sequence=index, result=None, error=None)
            clear(job)
            jobs[job['id']] = job
            event('created', job)
        for step, action in enumerate(SCENARIO['timeline'], 1):
            op, now = action['op'], action.get('now')
            if op == 'claim':
                for job in jobs.values():
                    if job['state'] == 'running' and now >= job['lease_until']:
                        job['state'] = 'queued' if job['attempts'] < job['max_attempts'] else 'failed'
                        job.update(available_at=now, error='lease expired')
                        clear(job)
                        event('lease_expired', job)
                eligible = [j for j in jobs.values() if j['state'] == 'queued'
                            and j['available_at'] <= now
                            and all(jobs[d]['state'] == 'succeeded' for d in j['depends_on'])]
                job = min(eligible, key=lambda j:(-j['priority'],j['sequence'])) if eligible else None
                if job:
                    job.update(state='running', attempts=job['attempts']+1, worker=action['worker'],
                               lease_token=str(step), lease_until=now+action['lease_seconds'], error=None)
                    last[action['worker']] = job['id']
                    event('claimed', job)
                claims.append(dict(step=step, id=job['id'] if job else None))
            elif op in ('complete', 'fail', 'heartbeat'):
                job = jobs[last[action['worker_ref']]]
                self.assertEqual(job['state'], 'running')
                self.assertLess(now, job['lease_until'])
                if op == 'heartbeat':
                    job['lease_until'] = max(job['lease_until'], now+action['lease_seconds'])
                    event('heartbeat', job)
                    continue
                if op == 'complete':
                    job.update(state='succeeded', result=action['result'])
                    kind = 'completed'
                else:
                    retry = job['attempts'] < job['max_attempts']
                    job.update(state='queued' if retry else 'failed', error=action['error'])
                    if retry:
                        job['available_at'] = now+action['retry_delay']
                    kind = 'retry_scheduled' if retry else 'failed'
                clear(job)
                event(kind, job)
            elif op in ('retry', 'cancel'):
                job = jobs[action['id']]
                if op == 'retry':
                    self.assertIn(job['state'], ('failed','cancelled'))
                    job.update(state='queued', attempts=0, available_at=action['available_at'],
                               result=None, error=None)
                else:
                    self.assertIn(job['state'], ('queued','running'))
                    job['state'] = 'cancelled'
                clear(job)
                event('retried' if op == 'retry' else 'cancelled', job)
            else:
                self.assertEqual(op, 'compact')
        oracle = expected()
        self.assertEqual(claims, oracle['claims'])
        self.assertEqual(events, oracle['events'])
        self.assertEqual(list(jobs.values()), oracle['jobs'])
        self.assertEqual(len(events), 46)

    def test_payload_variant_changes_results_without_mutating_original(self):
        source = copy.deepcopy(SCENARIO)
        variant = copy.deepcopy(source['jobs'])
        for job in variant:
            job['payload']['units'] += 11
        original, changed = expected(), expected(variant)
        self.assertEqual(original['claims'], changed['claims'])
        self.assertEqual(original['events'], changed['events'])
        for a, b in zip(original['groups'], changed['groups']):
            self.assertEqual(b['units']-a['units'], 11*a['jobs'])
        self.assertEqual(SCENARIO, source)


if __name__ == '__main__':
    unittest.main()
