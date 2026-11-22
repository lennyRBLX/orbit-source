import { createHash } from 'node:crypto';

export const sha256 = bytes => createHash('sha256').update(bytes).digest('hex');
export function validateArtifacts({ status, packageVersion, header, payload }) {
  const fail = message => { throw new Error(message); };
  if (status.version !== packageVersion || !header.includes(`#define ORBIT_VERSION "${status.version}"`))
    fail('Release, npm and payload versions do not match');
  if (payload.subarray(0, 4).toString('hex') !== '7f454c46') fail('Invalid ELF artifact');
  if (!payload.includes(Buffer.from(`Orbit-Store/${status.version}\0`))) fail('ELF has the wrong version');
}
