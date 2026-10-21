"""Exercise controller mounts, cgroup namespaces and ancestor CPU quotas."""
import argparse
from pathlib import Path
import subprocess
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--test', type=Path, required=True)
a = p.parse_args()
with tempfile.TemporaryDirectory(prefix='cpu-quota-', dir=a.test.resolve().parent) as tmp:
    root = Path(tmp)
    mount = root / 'controller with spaces'
    leaf = mount / 'worker'
    leaf.mkdir(parents=True)
    member, mounts = root / 'membership', root / 'mountinfo'
    escaped = mount.as_posix().replace(' ', r'\040')
    for version in [1, 2]:
        unified = version == 2
        member.write_text('0::/tenant/worker\n' if unified else '3:cpu,cpuacct:/tenant/worker\n')
        mounts.write_text(f'29 23 0:26 /tenant {escaped} rw - '
                          + ('cgroup2 cgroup rw\n' if unified else 'cgroup cgroup rw,cpu,cpuacct\n'))

        def quota(directory, q, period):
            if unified:
                (directory / 'cpu.max').write_text(f'{q} {period}\n')
            else:
                (directory / 'cpu.cfs_quota_us').write_text(f'{q}\n')
                (directory / 'cpu.cfs_period_us').write_text(f'{period}\n')

        def check(expected):
            subprocess.run([str(a.test.resolve()), 'quota', str(mounts), str(member), str(expected)], check=True, timeout=10)

        quota(mount, 200000, 100000)
        quota(leaf, 'max' if unified else -1, 100000)
        check(2)  # An unlimited child still obeys its parent.
        quota(leaf, 50000, 100000)
        check(1)
        quota(leaf, 150000, 100000)
        check(2)
        quota(leaf, 800000, 100000)
        check(2)
        quota(leaf, '99999999999999999999999999999999999', 0)
        check(2)
        member.write_text('0::/\n' if unified else '3:cpu,cpuacct:/\n')
        check(2)  # A namespaced root maps to the controller's mount root.
        member.write_text('0::/tenant/../outside\n' if unified else '3:cpu,cpuacct:/tenant/../outside\n')
        check(2)
    member.write_text('1:cpuset:/worker\n')
    subprocess.run([str(a.test.resolve()), 'quota', str(mounts), str(member), '32'], check=True, timeout=10)
print('PASS: 15 cgroup v1/v2 quota, fractional, namespace, escaped mount and ancestor cases')
