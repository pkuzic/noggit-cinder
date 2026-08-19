import os

print('Buidling...')

os.system('cmake -B./bin -DBLIZZARD_DATABASE_TEST_CONSOLE:BOOL=ON')

print('Done...')
