# corpus/

The validation corpus: small source trees with hand-written expected
answers, run by `scripts/corpus-run.py` (ctest `corpus` and
`corpus_selfcheck`). The contract — case layout, the expectation
language, the report format, how to add a case — is
`docs/validation.md`. `VERSION` is bumped when a case's expectations
change; `reports/baseline.json` is the report for the revision that
last touched the corpus.
