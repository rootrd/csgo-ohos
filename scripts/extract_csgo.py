import zstandard, tarfile, time, os

src = r'E:\csgo\CSGO-Source-Linux-20260928.tar.zst'
dst = r'E:\csgo'
log = r'E:\csgo\scripts\extract.log'

os.makedirs(os.path.dirname(log), exist_ok=True)

t0 = time.time()
with open(log, 'w') as lf:
    lf.write(f'extract start: {src}\n')
    lf.flush()
    n = 0
    with open(src, 'rb') as f:
        dctx = zstandard.ZstdDecompressor()
        reader = dctx.stream_reader(f)
        with tarfile.open(fileobj=reader, mode='r|') as tf:
            for m in tf:
                tf.extract(m, dst)
                n += 1
                if n % 2000 == 0:
                    lf.write(f'  {n} entries...\n')
                    lf.flush()
    lf.write(f'DONE: {n} entries in {time.time()-t0:.1f}s\n')
print(f'extract done {n} entries in {time.time()-t0:.1f}s')