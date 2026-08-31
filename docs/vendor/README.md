# Controlled Vendor Inputs

Vendor originals are not committed to this public repository because no redistribution grant is
on record. Install authorized copies at the paths listed in `SHA256SUMS`; the expected ESI path is
`docs/lz-joint/ECAT_CIA402.xml`.

Run `python3 tools/validate_project.py --root . --require-vendor-artifacts` before approving an
engineering release. Without the flag, missing controlled inputs are reported but do not fail a
public clean-checkout build. Any installed input with an incorrect hash always fails validation.
