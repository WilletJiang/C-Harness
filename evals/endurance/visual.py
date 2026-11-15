"""Image-only scenario facts layered onto the fixed Workboard release task."""
from pathlib import Path
import json
from . import task

IDS = ['collect-A', 'collect-B', 'collect-C', 'parse-A', 'parse-B', 'parse-C',
       'merge', 'audit', 'publish', 'cleanup', 'notify', 'archive']
DEPS = [[], [], [], ['collect-A'], ['collect-B'], ['collect-C'],
        ['parse-A', 'parse-B', 'parse-C'], ['merge'], ['audit'], ['publish'],
        ['publish'], ['cleanup', 'notify']]
PRIORITIES = [6, 9, 9, 4, 7, 5, 8, 3, 10, 1, 2, 0]
ATTEMPTS = [2, 2, 2, 2, 3, 2, 2, 1, 2, 2, 2, 2]
AVAILABLE = [0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0]
UNITS = [13, 21, 8, 34, 5, 17, 55, 3, 89, 2, 11, 7]
TEAMS = ['blue', 'amber', 'blue', 'blue', 'amber', 'blue',
         'amber', 'blue', 'amber', 'blue', 'amber', 'blue']
JOBS = [dict(id=id, depends_on=deps, priority=priority, max_attempts=attempts,
             available_at=at, payload=dict(team=team, units=units))
        for id, deps, priority, attempts, at, team, units in
        zip(IDS, DEPS, PRIORITIES, ATTEMPTS, AVAILABLE, TEAMS, UNITS)]


def claim(worker, now, lease):
    return dict(op='claim', worker=worker, now=now, lease_seconds=lease)


def complete(worker, now, label):
    return dict(op='complete', worker_ref=worker, now=now, result={'ok': label})


TIMELINE = [
    claim('w1', 0, 10), claim('w2', 0, 5), complete('w2', 1, 'A'),
    claim('w2', 2, 4), dict(op='fail', worker_ref='w1', now=3,
                           error='retry upstream', retry_delay=4),
    complete('w2', 4, 'C'), claim('w3', 4, 3), claim('w2', 4, 10),
    complete('w2', 5, 'PA'), claim('w1', 7, 8), complete('w1', 8, 'B'),
    claim('w2', 8, 10), complete('w2', 9, 'PB'), claim('w3', 9, 10),
    complete('w3', 10, 'PC'), claim('w1', 10, 10),
    dict(op='heartbeat', worker_ref='w1', now=11, lease_seconds=20),
    complete('w1', 12, 'M'), claim('w2', 12, 5),
    dict(op='fail', worker_ref='w2', now=13, error='manual review', retry_delay=0),
    claim('w1', 13, 10), dict(op='retry', id='audit', available_at=14),
    claim('w2', 14, 10), complete('w2', 15, 'AU'), claim('w1', 15, 10),
    complete('w1', 16, 'P'), dict(op='cancel', id='cleanup'), claim('w2', 16, 10),
    complete('w2', 17, 'N'), dict(op='retry', id='cleanup', available_at=18),
    claim('w3', 18, 10), complete('w3', 19, 'CL'), claim('w1', 19, 10),
    complete('w1', 20, 'AR'), dict(op='compact'),
]
SCENARIO = {'jobs': JOBS, 'timeline': TIMELINE}
IMAGES = ('01-workflow.png', '02-parameters.png', '03-timeline.png')
SPEC = '''# Image-based integration release

In addition to every requirement in SPEC.md, use the three supplied PNG images as
source data. The dependency arrows, task parameters and execution timeline exist
only in these images. Preserve the images, this file, SPEC.md and supplied tests.

Write scenario.json with exactly {"jobs":[...],"timeline":[...]}. Jobs appear in the
numbered table order; each is exactly {id,payload:{team,units},depends_on,priority,
max_attempts,available_at}. depends_on lists sorted incoming-arrow source IDs.
Copy timeline rows in order as their JSON objects. worker_ref means the id and
lease_token from that worker's most recent non-None claim; it is resolved at run
time, never hardcoded. retry/cancel use explicit IDs. No unlisted actions occur.

Implement `python demo.py SCENARIO_JSON --output DIRECTORY` using your Store and
query/planning APIs. DIRECTORY is fresh. Create jobs in table order, record initial
plan layers and blocked map, then execute every timeline action. Every claim,
including None, contributes {"step":one_based_timeline_index,"id":id_or_null}.
Close and reopen Store after every action; prove the jobs and events are unchanged.
Export twice at the end into export-a.json and export-b.json and compare bytes.

Write DIRECTORY/report.json with exactly these fields:
- layers: initial plan(jobs).
- initial_blocked: initial blocked(jobs).
- claims: the claim records described above.
- jobs: final complete public job records, in creation order.
- events: complete event history projected to {seq,kind,job_id}.
- groups: aggregate final jobs by payload.team in first-appearance order, with
  fields payload.team, jobs(count rows), units(sum payload.units), attempts(sum attempts).
- reopen_equal: true only if every reopen matched jobs and full event snapshots.
- export_stable: true only if the two export files are byte-identical.

Generate a verified sample in demo-output/ and add focused regression tests for
the image scenario. demo.py must read supplied JSON, not precomputed outcomes:
the evaluator will also change payload.units without changing execution semantics.
Document the scenario and how to reproduce it. Finish all requirements, not only
image transcription. Python3.9+ standard library only for the submitted code.
'''
PROMPT = task.PROMPT + ''' Also complete SCENARIO.md. The three attached images, in
order, are the dependency graph, numbered parameter table, and exact execution
timeline; identical PNGs are in references/. Read their visual content, transcribe
scenario.json, implement the reusable integration demo, and verify it with your
finished package. No network, OCR services or external packages are available.'''


def render(directory):
    # Pillow is a host-only fixture generator, never a runtime or submitted dependency.
    from PIL import Image, ImageDraw, ImageFont
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    font_path = '/System/Library/Fonts/Menlo.ttc'
    regular = ImageFont.truetype(font_path, 24)
    small = ImageFont.truetype(font_path, 22)
    title = ImageFont.truetype(font_path, 38)
    ink, muted, blue = '#122237', '#526578', '#2864ad'

    def canvas(size, heading, subtitle):
        image = Image.new('RGB', size, '#f6f8fc')
        draw = ImageDraw.Draw(image)
        draw.text((50, 32), heading, font=title, fill=ink)
        draw.text((50, 92), subtitle, font=regular, fill=muted)
        return image, draw

    image, draw = canvas((1500, 1420), '01 / Dependency graph',
                         'Arrow A -> B means B depends on A. Every arrow is direct.')
    centers = [(270, 220), (750, 220), (1230, 220),
               (270, 400), (750, 400), (1230, 400),
               (750, 580), (750, 760), (750, 940),
               (480, 1120), (1020, 1120), (750, 1300)]
    positions = dict(zip(IDS, centers))
    for job in JOBS:
        x2, y2 = positions[job['id']]
        for parent in job['depends_on']:
            x1, y1 = positions[parent]
            start, end = (x1, y1+37), (x2, y2-42)
            draw.line((start, end), fill=blue, width=5)
            import math
            angle = math.atan2(end[1]-start[1], end[0]-start[0])
            pts = [end] + [(end[0]-18*math.cos(angle+a), end[1]-18*math.sin(angle+a))
                           for a in (-0.42, 0.42)]
            draw.polygon(pts, fill=blue)
    for id, (x, y) in positions.items():
        draw.rounded_rectangle((x-135,y-36,x+135,y+36), radius=15,
                               fill='white', outline=ink, width=2)
        draw.text((x, y), id, font=regular, fill=ink, anchor='mm')
    image.save(directory / IMAGES[0])

    image, draw = canvas((1680, 1000), '02 / Numbered task parameters',
                         'Create in row order. Payload is exactly {team, units}.')
    columns = [(50, '#'), (125, 'id'), (450, 'priority'), (650, 'max_attempts'),
               (940, 'available_at'), (1230, 'team'), (1470, 'units')]
    for x, heading in columns:
        draw.text((x, 158), heading, font=small, fill=blue)
    for i, job in enumerate(JOBS):
        y = 210+i*57
        draw.rounded_rectangle((38,y-7,1630,y+43),radius=6,
                               fill='#e6edf6' if i%2==0 else 'white')
        values = [i+1, job['id'], job['priority'], job['max_attempts'],
                  job['available_at'], job['payload']['team'], job['payload']['units']]
        for (x, _), value in zip(columns, values):
            draw.text((x, y), str(value), font=regular, fill=ink)
    draw.text((50, 930), 'Dependencies come from image 01, not from table row adjacency.',
              font=regular, fill=muted)
    image.save(directory / IMAGES[1])

    image, draw = canvas((1880, 1900), '03 / Exact execution timeline',
                         'worker_ref = that worker\'s most recent non-null claim (id + token).')
    for i, action in enumerate(TIMELINE):
        y = 160+i*47
        draw.rectangle((35,y-5,1845,y+37), fill='#e6edf6' if i%2==0 else 'white')
        draw.text((50,y), f'{i+1:02d}', font=small, fill=blue)
        text = json.dumps(action, separators=(',', ':'))
        assert draw.textlength(text,font=small) < 1690
        draw.text((115,y), text, font=small, fill=ink)
    draw.text((50, 1830), 'Close/reopen after EVERY row. No unlisted actions. Times are logical timestamps.',
              font=small, fill=muted)
    image.save(directory / IMAGES[2])


def prepare(workspace):
    task.prepare(workspace)
    workspace = Path(workspace)
    (workspace / 'SCENARIO.md').write_text(SPEC)
    render(workspace / 'references')
    return PROMPT
