import os
import yaml

base_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
workflow_path = os.environ.get('WORKFLOW_PATH', os.path.join(base_dir, '.github/workflows/build-o1s.yml'))
with open(workflow_path, 'r') as f:
    data = yaml.safe_load(f)

print('=== VALIDATING GITHUB WORKFLOW YAML ===')
print('Workflow name:', data.get('name'))
on_block = data.get('on') if data.get('on') is not None else data.get(True, {})
paths = on_block.get('push', {}).get('paths', [])
print(f'Paths filter count: {len(paths)}')
for p in paths:
    print(f'  - {p}')

steps = data.get('jobs', {}).get('build', {}).get('steps', [])
print(f'Steps count: {len(steps)}')

assert len(paths) >= 15, f'Paths filter count too low: {len(paths)}'
assert len(steps) >= 15, f'Steps count too low: {len(steps)}'
print('CI YAML VALIDATION PASSED!')
