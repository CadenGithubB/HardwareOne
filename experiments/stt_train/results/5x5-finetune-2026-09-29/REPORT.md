# Overnight fine-tuning report

| Test set | Before WER | After WER |
|---|---:|---:|
| ami-sdm-test | 79.2% | 68.7% |
| ami-ihm-test | 53.3% | 43.0% |
| librispeech-test-clean | 5.6% | 7.5% |

## Example transcripts (after)

### ami-sdm-test
- ref: okay
  hyp: 
- ref: yeah yeah
  hyp: yeah
- ref: exactly
  hyp: try
- ref: yeah
  hyp: yeah
- ref: yeah
  hyp: yeah
- ref: okay
  hyp: okay
- ref: yeah
  hyp: 
- ref: but yeah
  hyp: yeah
- ref: okay
  hyp: toa
- ref: thank you
  hyp: yeh
- ref: mm hmm
  hyp: 
- ref: yeah yeah
  hyp: yeah
### ami-ihm-test
- ref: cutest
  hyp: cu
- ref: think we do
  hyp: ocu
- ref: right
  hyp: right
- ref: okay
  hyp: oka
- ref: i mean
  hyp: i mean
- ref: mm
  hyp: ahrgh
- ref: that
  hyp: yeh
- ref: yeah
  hyp: yeah
- ref: exactly
  hyp: jacl
- ref: um
  hyp: um
- ref: mm hmm
  hyp: yeah
- ref: yes
  hyp: yeah
### librispeech-test-clean
- ref: a story
  hyp: a story
- ref: direction
  hyp: direction
- ref: verse two
  hyp: first tomb
- ref: oh emil
  hyp: ow amil
- ref: indeed ah
  hyp: indeed uh
- ref: farewell madam
  hyp: farewell madame
- ref: poor alice
  hyp: poor alice
- ref: there just in front
  hyp: there just in front
- ref: hans stirs not
  hyp: han stars not
- ref: venice
  hyp: vhenis
- ref: marie sighed
  hyp: muri sighed
- ref: what was that
  hyp: what was that
