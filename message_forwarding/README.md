# HERCULES/HERACLES PLUGIN

## Message forwarding
With message forwarding the user can setup the current online character to receive the messages sent to other characters of the same account.

When sending messages to a character with message forwarding enabled, the server will respond with the following message:
> Your message has been forwarded to (<online_char_name>)

## Commands

There are two commands available:

#### @mesfw

`@mesfw <char_name>` Enables message forwarding for the character specified.

`@mesfw <char_name> stop` Disables message forwarding for the character specified.

#### @mesfwall

`@mesfwall` Enables message forwarding for all characters in the current account.

`@mesfwall list` Displays the list of characters with message forwarding enabled.

`@mesfwall stop` Disables message forwarding for all characters in the current account.