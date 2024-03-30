# HERCULES/HERACLES PLUGIN

## Autodelete monster
Enables summoning monsters that will be removed, if not killed previously, after the specified time in seconds.

### Syntax
The syntax is very similar to the monster script command, except for the eighth position, which determines the duration of the monster, in seconds:

```
admonster <map name>,<x>,<y>,<xs>,<ys>%TAB%monster%TAB%<monster name>%TAB%<mob id>,<amount>,<delay1>,<delay2>,<duration>{,<event>,<mob size>,<mob ai>}
```
Dead branch example that auto deletes monsters in 1 hour:

```
admonster "this",-1,-1,"--ja--",-1,1,3600,"";
```