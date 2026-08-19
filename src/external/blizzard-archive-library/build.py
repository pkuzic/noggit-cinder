import os

print('Buidling...')

os.system('cmake -B./bin -DBLIZZARD_ARCHIVE_TEST_CONSOLE:BOOL=ON' )

print('Done...')
