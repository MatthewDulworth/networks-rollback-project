A simple simple two-player 2D fighting game built in C with rollback netcode for online play over UDP. The game itself will be minimal: two fighters that can move, jump and attack so that I can focus on the network aspects.

Fighting games run at a fixed 60 frames per second, and players need their inputs to take effect within a frame or two. Traditional delaye-based netcode waits for each players input before simulating each frame. As a result, input lag grows with ping, lost/late packets can freeze the game. Rollback netcode (now the standard) avoids that wait.

Rollback overview:
- The two peers connect directly over UDP and send each other only their controller inputs. Both machines run a completly identical simulation, so the same inputs always produce the same game.
- If the opponent's input for a frame hasn't arrived yet, the game guesses it (by repeating their last input) and keeps running. Each frame's state is saved. When the real input arrives and doesn't match the guess, the game reloads the saved state from that frame and re-simulates forward to the present. This way the player only sees a small correction instead of a freeze.
- Every packet includes all inputs the other peer hasn't acknowledged yet, so there are no timeouts/resend, no need for TCP.
- Peers exchange how many frames ahead of each other they are. The one running ahead slows down slightly so that neither side does all the predicting.
- Peers periodically exchange a hash of the game state to confirm their two copies of the game still match. 

For testing I will write a wrapper that can add artificial latenecy/packet loss to the network communications. If I have time I will also create a second version of the game that uses a traditional delay-based method to compare to the rollback approach in bad network conditions.

Will be implemented in C, POSIX sockets for networking, raylib for graphics and input, no rollback or game-networking specific libraries will be used.

references:
- Rollback broad overview: https://words.infil.net/w01-netcode.html
- Open source rollback implementation for reference: https://github.com/pond3r/ggpo
