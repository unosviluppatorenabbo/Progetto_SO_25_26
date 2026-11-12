int main() {
    int numeri[10] = (1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
    int somma = 0;

    for(size_t i = 0;i < 10;i++) {
        somma += numeri[i];
    }

    while(!condizione) { // prima controlla la condizione e poi esegue
 
    }

    do {    // esegue almeno una volta prima di controllare la condizione

    } while(condizione);

    .data
        arr: .word 1, 2, 3, 4, 5, 6, 7, 8, 9, 10
        n: .word 10

    .global_start

    la t0, arr
    la t1, n
    lw t1, 0(t1)

    addi a0, zero, 0
    
    LOOP: bleqz, t1, ENDLOOP
        lw t2, 0(t0)
        add a0, a0, t2

        addi t0, t0, 4
        addi t1, t1, -1
        J LOOP
    ENDLOOP:

    int g[5] = (5, 2, 8, 9, 3);
    int t = 20;

    for(size_t i = 0; i < 5 && t > 0; i++){
        t -= g[i];
    }

    .data
        arr: .word 5, 2, 8, 9, 3
        n: .word 5

    .global_start
    la t0, arr
    la t1, n
    lw t1, 0(t1)
    addi a0, zero, 20

    LOOP: bleqz t1, ENDLOOP
        bleqz a0, ENDLOOP
        lw t2, 0(t0)
        sub a0, a0, t2

        addi t0, t0, 4
        addi t1, t1, -1
        J LOOP

    ENDLOOP:


}