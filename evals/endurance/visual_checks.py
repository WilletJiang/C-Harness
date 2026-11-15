"""Frozen image transcription and integration acceptance, independent of submitted tests."""
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from .visual import JOBS, TIMELINE, IDS, DEPS

CLAIMS = list(zip([1, 2, 4, 7, 8, 10, 12, 14, 16, 19, 21, 23, 25, 28, 31, 33],
                  ['collect-B','collect-A','collect-C','parse-C','parse-A','collect-B',
                   'parse-B','parse-C','merge','audit',None,'audit','publish','notify',
                   'cleanup','archive']))
LAYERS = [IDS[:3], IDS[3:6], ['merge'], ['audit'], ['publish'], ['cleanup','notify'], ['archive']]
EVENTS = [('created', id) for id in IDS] + [
    ('claimed','collect-B'), ('claimed','collect-A'), ('completed','collect-A'),
    ('claimed','collect-C'), ('retry_scheduled','collect-B'), ('completed','collect-C'),
    ('claimed','parse-C'), ('claimed','parse-A'), ('completed','parse-A'),
    ('lease_expired','parse-C'), ('claimed','collect-B'), ('completed','collect-B'),
    ('claimed','parse-B'), ('completed','parse-B'), ('claimed','parse-C'), ('completed','parse-C'),
    ('claimed','merge'), ('heartbeat','merge'), ('completed','merge'), ('claimed','audit'),
    ('failed','audit'), ('retried','audit'), ('claimed','audit'), ('completed','audit'),
    ('claimed','publish'), ('completed','publish'), ('cancelled','cleanup'), ('claimed','notify'),
    ('completed','notify'), ('retried','cleanup'), ('claimed','cleanup'), ('completed','cleanup'),
    ('claimed','archive'), ('completed','archive'),
]


def expected(jobs=JOBS):
    records = []
    labels = ['A','B','C','PA','PB','PC','M','AU','P','CL','N','AR']
    changed = {'collect-B':7, 'parse-C':7, 'audit':14, 'cleanup':18}
    groups = {}
    for i, (source, label) in enumerate(zip(jobs, labels), 1):
        job = copy.deepcopy(source)
        job.update(state='succeeded', attempts=2 if job['id'] in ('collect-B','parse-C') else 1,
                   available_at=changed.get(job['id'], job['available_at']), sequence=i,
                   lease_token=None, lease_until=None, worker=None, error=None, result={'ok':label})
        records.append(job)
        team = job['payload']['team']
        group = groups.setdefault(team, {'payload.team':team,'jobs':0,'units':0,'attempts':0})
        group['jobs'] += 1
        group['units'] += job['payload']['units']
        group['attempts'] += job['attempts']
    return {'layers':LAYERS, 'initial_blocked':{id:deps for id,deps in zip(IDS,DEPS) if deps},
            'claims':[{'step':step,'id':id} for step,id in CLAIMS], 'jobs':records,
            'events':[{'seq':i,'kind':kind,'job_id':id} for i,(kind,id) in enumerate(EVENTS,1)],
            'groups':list(groups.values()),'reopen_equal':True,'export_stable':True}


def demo(workspace, scenario, root):
    input_path = workspace / '.evaluation-scenario.json'
    # Place only the scenario, never expected outcomes, within the allowed workspace.
    input_path.write_text(json.dumps(scenario))
    output = workspace / root
    try:
        process = subprocess.run([sys.executable,'-B',str(workspace/'demo.py'),str(input_path),
                                  '--output',str(output)], cwd=workspace, capture_output=True,
                                 text=True, timeout=25)
        if process.returncode:
            return {}, {'exit_code':process.returncode,'stderr':process.stderr[-1500:]}
        value = json.loads((output/'report.json').read_text())
        a,b = (output/'export-a.json').read_bytes(),(output/'export-b.json').read_bytes()
        export = json.loads(a)
        stable = a==b and export.get('format')==1 and export.get('jobs')==value.get('jobs')
        projected = [{k:e[k] for k in ('seq','kind','job_id')} for e in export.get('events',[])]
        stable = stable and projected == value.get('events')
        return value, {'export_verified':stable}
    except Exception as error:
        return {}, {'grader_error':type(error).__name__+': '+str(error)[:500]}
    finally:
        input_path.unlink(missing_ok=True)


def grade(workspace):
    workspace = Path(workspace).resolve()
    results = []
    def check(name, actual, wanted):
        results.append({'name':name,'pass':actual==wanted,
                        **({} if actual==wanted else {'expected':wanted,'actual':actual})})
    try:
        scenario = json.loads((workspace/'scenario.json').read_text())
    except Exception:
        scenario = {}
    check('scenario_shape', sorted(scenario), ['jobs','timeline'])
    jobs = scenario.get('jobs',[])
    for i, job in enumerate(JOBS):
        check('image_job_'+job['id'], jobs[i] if i<len(jobs) else None, job)
    check('image_timeline',scenario.get('timeline'),TIMELINE)
    # Grade in a disposable workspace copy; submitted artifacts remain unchanged.
    import shutil
    with tempfile.TemporaryDirectory(prefix='ah-image-grade-') as temporary:
        copy_dir = Path(temporary)/'workspace'
        shutil.copytree(workspace,copy_dir,ignore=shutil.ignore_patterns('__pycache__','.git'))
        report, diagnostics = demo(copy_dir,scenario,'.evaluation-original')
        wanted = expected()
        check('report_shape', sorted(report), sorted(wanted))
        for key in wanted:
            check('demo_'+key, report.get(key), wanted[key])
        check('export_files',diagnostics.get('export_verified'),True)
        alternate = {'jobs':copy.deepcopy(JOBS),'timeline':TIMELINE}
        for job in alternate['jobs']:
            job['payload']['units'] += 11
        changed, changed_diagnostics = demo(copy_dir,alternate,'.evaluation-variant')
        check('generalized_payload',changed,expected(alternate['jobs']))
        check('generalized_export',changed_diagnostics.get('export_verified'),True)
    return {'passed':sum(r['pass'] for r in results),'total':len(results),
            'checks':results,'execution_diagnostics':diagnostics,
            'variant_diagnostics':changed_diagnostics}


if __name__=='__main__':
    print(json.dumps(grade(sys.argv[1]),ensure_ascii=False))
