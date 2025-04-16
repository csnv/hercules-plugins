# HERCULES/HERACLES PLUGIN
> [!NOTE]
> This plugin is in an alpha state. Be aware of duplication exploits and report any bugs you find!

## Parallel autotrade
Parallel autotrade is a replacement for the `@autotrade` command that allows users to keep playing in the same account as their vending character. It's compatible with vending and buyinstores, as well as the searchstore functionality.

### Main differences with default `@autotrade`
- Allows users to play in other characters from the same account
- Allows several autotrade merchants or buyingstores per account
- Blazingly fast loading of merchants when persistency is enabled

> [!WARNING]
> Buyingstore functionality is only available in Heracles. If you're using Hercules, you need to apply these patches:
> - https://github.com/HeraclesHub/Heracles/pull/20/commits/a6324c524f34792236900b8338ff2e733c7de230
> - https://github.com/HeraclesHub/Heracles/pull/21/commits/14edcf0251fd3860b0c30c3eb1e64a7fa2d5ca21
> 
> `@whosell` and `@whobuy` must be updated to work with databases.